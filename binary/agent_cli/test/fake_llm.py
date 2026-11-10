#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""固定的假 LLM：不管收到什么，一律回「任务已成功」。

用途：让 agent_cli / agent_cli_server / 管理台能在**完全离线**的条件下跑通全链路，
不用真模型、不用写死某个端点的剧本。agent_cli 强制 stream:true，所以默认走 OpenAI
兼容的 SSE(chunked) 回包；stream:false 时返回普通整包 JSON。

单独常驻：
    python binary/agent_cli/test/fake_llm.py --port 8732
    # 把 agent_cli 的 llm.url 指到 http://127.0.0.1:8732/v1/chat/completions

被别的脚本复用：
    from fake_llm import start_fake_llm
    server, url = start_fake_llm(reply="任务已成功", delay=0.2)
"""
from __future__ import annotations

import argparse
import json
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

DEFAULT_REPLY = "任务已成功"


class FakeLlmHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    reply = DEFAULT_REPLY
    delay = 0.0
    hits = 0

    def log_message(self, fmt, *args):  # noqa: D102
        pass

    def _read_body(self) -> bytes:
        # JZNet/mongoose 对较大的 POST 用 chunked 发送，没有 Content-Length。
        if "chunked" in (self.headers.get("Transfer-Encoding") or "").lower():
            data = b""
            while True:
                line = self.rfile.readline().strip()
                if not line:
                    break
                size = int(line.split(b";")[0], 16)
                if size == 0:
                    self.rfile.readline()
                    break
                data += self.rfile.read(size)
                self.rfile.read(2)
            return data
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n) if n else b""

    def do_GET(self):  # noqa: N802
        if self.path.startswith("/health"):
            body = json.dumps({"ok": True, "reply": type(self).reply,
                               "hits": type(self).hits}, ensure_ascii=False).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        self.send_response(404)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def _chunks(self, reply: str) -> list:
        head = {
            "id": "fake", "object": "chat.completion.chunk", "created": 0, "model": "fake",
            "choices": [{"index": 0, "delta": {"content": reply}, "finish_reason": None}],
            "usage": {"prompt_tokens": 1, "completion_tokens": 1, "total_tokens": 2},
        }
        tail = {
            "id": "fake", "object": "chat.completion.chunk", "created": 0, "model": "fake",
            "choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}],
        }
        return [json.dumps(head, ensure_ascii=False).encode("utf-8"),
                json.dumps(tail, ensure_ascii=False).encode("utf-8")]

    def do_POST(self):  # noqa: N802
        raw = self._read_body()
        try:
            body = json.loads(raw.decode("utf-8")) if raw else {}
        except Exception:  # noqa: BLE001
            body = {}
        type(self).hits += 1
        if type(self).delay > 0:
            time.sleep(type(self).delay)
        reply = type(self).reply

        if not body.get("stream", True):
            payload = {
                "id": "fake", "object": "chat.completion", "created": 0, "model": "fake",
                "choices": [{"index": 0, "message": {"role": "assistant", "content": reply},
                             "finish_reason": "stop"}],
                "usage": {"prompt_tokens": 1, "completion_tokens": 1, "total_tokens": 2},
            }
            data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return

        try:
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream; charset=utf-8")
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()

            def write_chunk(data: bytes) -> None:
                self.wfile.write(("%x\r\n" % len(data)).encode("ascii") + data + b"\r\n")
                self.wfile.flush()

            for chunk in self._chunks(reply):
                write_chunk(b"data: " + chunk + b"\n\n")
            write_chunk(b"data: [DONE]\n\n")
            self.wfile.write(b"0\r\n\r\n")
            self.wfile.flush()
        except OSError:
            pass  # 对端取消是正常路径


def start_fake_llm(reply: str = DEFAULT_REPLY, delay: float = 0.0,
                   host: str = "127.0.0.1", port: int = 0):
    """起一个后台假 LLM，返回 (server, url)。port=0 由系统分配。"""
    handler = type("ConfiguredFakeLlm", (FakeLlmHandler,),
                   {"reply": reply, "delay": delay, "hits": 0})
    server = ThreadingHTTPServer((host, port), handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    url = "http://%s:%d/v1/chat/completions" % (host, server.server_address[1])
    return server, url


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="固定返回「任务已成功」的假 LLM")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=0, help="0 = 随机端口")
    ap.add_argument("--reply", default=DEFAULT_REPLY)
    ap.add_argument("--delay", type=float, default=0.0, help="每个请求的人为延迟(秒)")
    args = ap.parse_args(argv)
    server, url = start_fake_llm(args.reply, args.delay, args.host, args.port)
    print("fake_llm: %s  reply=%r" % (url, args.reply), flush=True)
    try:
        threading.Event().wait()
    except KeyboardInterrupt:
        pass
    server.shutdown()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
