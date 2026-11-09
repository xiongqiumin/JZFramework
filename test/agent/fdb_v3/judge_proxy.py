#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
把上游 gpt-4o judge 接到任意 OpenAI 兼容端点的小代理（只改写 model 名）。

为什么需要：上游 evaluate_*.py 里 judge 的模型名写死 `model="gpt-4o"`，
而本地 vllm 只认它自己 serve 的名字，直接指过去会 404。
本代理在中间把 body 的 model 字段改写掉，其余原样转发，于是：

    set OPENAI_BASE_URL=http://127.0.0.1:18100/v1
    set OPENAI_API_KEY=<上游端点的 key>
    python evaluate_tool_calls.py ... --use-llm        # judge 实际跑在内网模型上

⚠ 只用标准库（不装额外依赖），只支持非流式（judge 本来就是非流式）。

用法：
    python judge_proxy.py --listen 18100 ^
        --target http://<模型主机>:8000/v1/chat/completions ^
        --model <模型名> --api-key <key>
"""

import argparse
import json
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ARGS = None


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *a):        # 静音默认访问日志
        pass

    def _send(self, code, body: bytes, ctype="application/json"):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path.rstrip("/").endswith("/models"):
            body = json.dumps({"object": "list", "data": [
                {"id": "gpt-4o", "object": "model", "owned_by": "proxy"}]}).encode()
            self._send(200, body)
        else:
            self._send(404, b'{"error":{"message":"not found"}}')

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(n)
        try:
            body = json.loads(raw.decode("utf-8"))
        except Exception:
            self._send(400, b'{"error":{"message":"bad json"}}')
            return

        body["model"] = ARGS.model          # ← 唯一改动：模型名
        data = json.dumps(body, ensure_ascii=False).encode("utf-8")
        req = urllib.request.Request(ARGS.target, data=data, method="POST")
        req.add_header("Content-Type", "application/json")
        if ARGS.api_key:
            req.add_header("Authorization", "Bearer " + ARGS.api_key)
        try:
            with urllib.request.urlopen(req, timeout=ARGS.timeout) as r:
                self._send(r.status, r.read())
        except urllib.error.HTTPError as e:
            body = e.read()
            if ARGS.verbose:
                print(f"[proxy] HTTP {e.code}: {body[:200]!r}")
            self._send(e.code, body)
        except Exception as e:
            print(f"[proxy] {type(e).__name__}: {e}")
            self._send(502, json.dumps({"error": {"message": str(e)}}).encode())


def main():
    global ARGS
    ap = argparse.ArgumentParser(description="OpenAI 兼容端点的 model 名改写代理")
    ap.add_argument("--listen", type=int, default=18100)
    ap.add_argument("--target", required=True, help="例如 http://<模型主机>:8000/v1/chat/completions")
    ap.add_argument("--model", required=True, help="目标端点认的模型名")
    ap.add_argument("--api-key", default="")
    ap.add_argument("--timeout", type=float, default=180.0)
    ap.add_argument("--verbose", action="store_true")
    ARGS = ap.parse_args()

    srv = ThreadingHTTPServer(("127.0.0.1", ARGS.listen), Handler)
    print(f"[proxy] 127.0.0.1:{ARGS.listen} -> {ARGS.target} (model={ARGS.model})")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
