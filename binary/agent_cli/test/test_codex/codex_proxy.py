#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""codex_proxy.py —— 给 Codex 接本机 llama.cpp 用的一层"角色适配"代理。

为什么必须有它(实测出来的, 不是猜的)
------------------------------------------------------------
Codex 走的是 **Responses API**(`POST /v1/responses`), 而且**每一轮**都会在 `input` 里带一条
`role="developer"` 的消息(OpenAI 新协议角色)。本机 llama.cpp 把 Responses 转成 chat messages
时, 会把 `developer` 映射成 **system**; 而当前模型的 Jinja 模板里有

    raise_exception('System message must be at the beginning.')

这条守卫 —— system 不在开头就直接 **HTTP 500**:
`Jinja Exception: System message must be at the beginning.`
(Codex 那边只会显示 "We're currently experiencing high demand…" 之类的通用文案, 极具误导性;
 真实状态码要去 <CODEX_HOME>/logs_2.sqlite 或这层代理的日志里看。)

⇒ 这层代理把 `input` 里 `role="developer"` 的项**改写成 role="user"**, 上游就正常了。

它刻意只做两件事(保持"薄", 便于对照)
------------------------------------------------------------
1. **改写请求体**: developer -> user(顺手统计改写了多少条, 用来确认适配真的生效);
2. **原样透传响应**: SSE 边收边发(不缓冲整条回复 —— 否则 Codex 会看不到流式输出);
   只顺手把 `usage` 摘出来做统计, 不改内容。

用法
------------------------------------------------------------
独立跑(调试用, 会把每轮的角色与 usage 打到日志):
    python codex_proxy.py --upstream http://192.168.111.1:8080/v1/chat/completions
    # 打印出的 base_url(形如 http://127.0.0.1:PORT/v1)填进 Codex 的
    #   model_providers.<name>.base_url, 并且 model_providers.<name>.wire_api=responses

被 test_codex_bench.py 当库用: `with CodexProxy(upstream) as p: ... p.base_url ... p.stats()`。
"""
from __future__ import annotations

import argparse
import http.client
import json
import os
import re
import socket
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit

# Codex 每轮都会发的角色 -> 上游能接受的角色。要再适配别的写法就往这里加。
ROLE_REWRITE = {"developer": "user"}

# 从 SSE 里摘 usage 用的(只做统计, 不参与转发)
_USAGE_RE = re.compile(r'"usage"\s*:\s*\{[^{}]*\}')


def responses_url_from_chat_url(chat_url: str) -> str:
    """把 chat/completions 的地址换成同一台服务的 Responses 地址。

    test_llm.json 里存的是 chat/completions(agent_cli 用那条); Codex 要 /v1/responses。
    """
    u = chat_url.strip().rstrip("/")
    if u.endswith("/chat/completions"):
        u = u[: -len("/chat/completions")]
    elif u.endswith("/v1"):
        pass
    return u + "/responses"


class CodexProxy:
    """developer->user 适配 + 透传代理。线程安全, 可随时 stats()/reset_stats()。"""

    def __init__(self, upstream: str, port: int = 0, log_path: str = "", timeout: float = 600.0):
        self.upstream = upstream
        self.request_url = responses_url_from_chat_url(upstream)
        parts = urlsplit(self.request_url)
        self._host = parts.hostname or "127.0.0.1"
        self._port = parts.port or 80
        self._path = parts.path or "/v1/responses"
        self._timeout = timeout
        self.log_path = log_path

        self._lock = threading.Lock()
        self._stats = {
            "requests": 0,
            "role_rewrites": 0,      # developer -> user 改写了多少条(0 说明适配没生效/Codex 换了协议)
            "failures": 0,
            "prompt_tokens": 0,
            "completion_tokens": 0,
            "cached_tokens": 0,
        }

        self._log_fp = None
        if log_path:
            os.makedirs(os.path.dirname(os.path.abspath(log_path)), exist_ok=True)
            self._log_fp = open(log_path, "a", encoding="utf-8")

        handler = self._make_handler()
        self._httpd = ThreadingHTTPServer(("127.0.0.1", port), handler)
        self._httpd.daemon_threads = True          # 主线程退出时不留悬挂的请求线程
        self.port = self._httpd.server_address[1]
        self._thread = threading.Thread(target=self._httpd.serve_forever,
                                        kwargs={"poll_interval": 0.2}, daemon=True)

    # ---------------------------------------------------------------- 生命周期
    @property
    def base_url(self) -> str:
        """填给 Codex 的 model_providers.<name>.base_url(Codex 自己会拼 /responses)。"""
        return "http://127.0.0.1:%d/v1" % self.port

    def start(self) -> "CodexProxy":
        self._thread.start()
        self.note("proxy 起在 %s -> %s" % (self.base_url, self.request_url))
        return self

    def stop(self) -> None:
        try:
            self._httpd.shutdown()
            self._httpd.server_close()
        except Exception:                                            # noqa: BLE001
            pass
        if self._log_fp:
            self._log_fp.flush()
            self._log_fp.close()
            self._log_fp = None

    def __enter__(self) -> "CodexProxy":
        return self.start()

    def __exit__(self, *exc) -> None:
        self.stop()

    # ---------------------------------------------------------------- 统计 / 日志
    def note(self, text: str) -> None:
        line = "[%s] %s" % (time.strftime("%H:%M:%S"), text)
        if self._log_fp:
            self._log_fp.write(line + "\n")
            self._log_fp.flush()

    def stats(self) -> dict:
        with self._lock:
            return dict(self._stats)

    def reset_stats(self) -> None:
        with self._lock:
            for k in self._stats:
                self._stats[k] = 0

    def _bump(self, key: str, n: int = 1) -> None:
        with self._lock:
            self._stats[key] = self._stats.get(key, 0) + n

    def _absorb_usage(self, text: str) -> None:
        """从 SSE data 行里摘 usage(统计用): llama.cpp 在 response.completed 里给 input/output tokens。"""
        for m in _USAGE_RE.finditer(text):
            try:
                u = json.loads(m.group(0).split(":", 1)[1])
            except ValueError:
                continue
            if not isinstance(u, dict):
                continue
            for key, field in (("prompt_tokens", "input_tokens"), ("completion_tokens", "output_tokens")):
                v = u.get(field)
                if isinstance(v, int) and v > 0:
                    self._bump(key, v)
            det = u.get("input_tokens_details") or u.get("prompt_tokens_details")
            if isinstance(det, dict) and isinstance(det.get("cached_tokens"), int):
                self._bump("cached_tokens", det["cached_tokens"])

    # ---------------------------------------------------------------- 请求处理
    def _make_handler(proxy):                                       # noqa: N805 (闭包更直观)
        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"
            server_version = "jz-codex-proxy/1.0"

            def log_message(self, fmt, *args):                       # 默认往 stderr 刷: 收到代理日志里
                proxy.note("http " + (fmt % args))

            def do_GET(self):                                        # noqa: N802
                body = json.dumps({"ok": True, "stats": proxy.stats()}, ensure_ascii=False)
                raw = body.encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json; charset=utf-8")
                self.send_header("Content-Length", str(len(raw)))
                self.end_headers()
                self.wfile.write(raw)

            def do_POST(self):                                       # noqa: N802
                try:
                    raw = self.rfile.read(int(self.headers.get("Content-Length", "0") or 0))
                except Exception as e:                               # noqa: BLE001
                    return self._fail("读请求体失败: %s" % e)
                proxy._bump("requests")
                try:
                    req = json.loads(raw.decode("utf-8"))
                except Exception as e:                               # noqa: BLE001
                    return self._fail("请求体不是 JSON: %s" % e)
                if not isinstance(req, dict):
                    return self._fail("请求体不是 JSON 对象")

                # ── ① 角色适配: developer -> user (代理存在的全部理由) ──
                rewritten = proxy._rewrite_roles(req)
                body = json.dumps(req, ensure_ascii=False).encode("utf-8")
                if rewritten:
                    proxy.note("POST %s model=%s role_rewrite=%d" % (self.path, req.get("model"), rewritten))
                else:
                    proxy.note("POST %s model=%s role_rewrite=0 (没有 developer 项?)" % (self.path, req.get("model")))

                # ── ② 转发 + 流式透传 ──
                headers = {
                    "Content-Type": "application/json",
                    "Accept": self.headers.get("Accept", "text/event-stream"),
                    "Content-Length": str(len(body)),
                }
                if self.headers.get("Authorization"):
                    headers["Authorization"] = self.headers["Authorization"]
                try:
                    conn = http.client.HTTPConnection(proxy._host, proxy._port, timeout=proxy._timeout)
                    conn.request("POST", proxy._path, body=body, headers=headers)
                    resp = conn.getresponse()
                except Exception as e:                               # noqa: BLE001
                    return self._fail("连不上上游 %s: %s" % (proxy.request_url, e))

                self.send_response(resp.status)
                hop_by_hop = {"transfer-encoding", "connection", "keep-alive", "content-length"}
                for k, v in resp.getheaders():
                    if k.lower() in hop_by_hop:
                        continue
                    self.send_header(k, v)
                length = resp.getheader("Content-Length")
                if length is not None:
                    self.send_header("Content-Length", length)
                    self.end_headers()
                    while True:
                        chunk = resp.read(8192)
                        if not chunk:
                            break
                        self.wfile.write(chunk)
                    self.wfile.flush()
                else:
                    # 未知长度(SSE): 自己打 chunked 帧, 边收边发 —— 不能攒完再发(Codex 要看流)
                    self.send_header("Transfer-Encoding", "chunked")
                    self.end_headers()
                    try:
                        while True:
                            chunk = resp.read(4096)
                            if not chunk:
                                break
                            proxy._absorb_usage(chunk.decode("utf-8", "replace"))
                            self.wfile.write(b"%x\r\n" % len(chunk) + chunk + b"\r\n")
                            self.wfile.flush()
                        self.wfile.write(b"0\r\n\r\n")
                        self.wfile.flush()
                    except (BrokenPipeError, ConnectionResetError):
                        proxy.note("客户端断了(Codex 取消/超时?)")
                if resp.status >= 400:
                    proxy._bump("failures")
                    proxy.note("上游返回 %d: %s" % (resp.status, resp.reason))
                conn.close()

            def _fail(self, text):
                proxy._bump("failures")
                proxy.note("代理错误: " + text)
                raw = json.dumps({"error": {"message": text, "type": "proxy_error"}},
                                 ensure_ascii=False).encode("utf-8")
                try:
                    self.send_response(502)
                    self.send_header("Content-Type", "application/json; charset=utf-8")
                    self.send_header("Content-Length", str(len(raw)))
                    self.end_headers()
                    self.wfile.write(raw)
                except Exception:                                    # noqa: BLE001
                    pass

        return Handler

    def _rewrite_roles(self, req: dict) -> int:
        """把 input 里需要适配的 role 改掉(dict 内联改写), 返回改写条数。

        ⚠ 只动 `input`(Codex 真正发消息的地方); 顶层 `instructions` 是系统提示, llama.cpp 会
          映射成开头的 system ✓ 不用动。input 是字符串时没有角色可改(新会话第一轮才这样)。
        """
        n = 0
        items = req.get("input")
        if isinstance(items, list):
            for it in items:
                if isinstance(it, dict):
                    role = it.get("role")
                    if isinstance(role, str) and role in ROLE_REWRITE:
                        it["role"] = ROLE_REWRITE[role]
                        n += 1
        if n:
            self._bump("role_rewrites", n)
        return n


def _free_port() -> int:
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Codex -> llama.cpp 角色适配代理(developer 改写成 user)")
    ap.add_argument("--upstream", required=True,
                    help="上游 chat/completions 地址(自动换成同服务的 /responses)")
    ap.add_argument("--port", type=int, default=0, help="本地监听端口(默认自动挑一个)")
    ap.add_argument("--log", default="", help="日志文件(默认只打 stdout)")
    ap.add_argument("--timeout", type=float, default=600.0, help="上游请求超时(秒)")
    ap.add_argument("--system-to-user", action="store_true",
                    help="把 input 里 role=system 的项也改写成 user(备用: 若上游又报 system 必须在开头)")
    args = ap.parse_args(argv)

    if args.system_to_user:
        ROLE_REWRITE["system"] = "user"

    proxy = CodexProxy(args.upstream, args.port, args.log, args.timeout)
    print("base_url   : %s" % proxy.base_url, flush=True)
    print("responses  : %s" % proxy.request_url, flush=True)
    print("(把 base_url 填进 Codex 的 model_providers.<name>.base_url, wire_api=responses)", flush=True)
    proxy.start()
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        pass
    finally:
        proxy.stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
