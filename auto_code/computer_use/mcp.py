#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
computer_use 脚手架 —— 极简 MCP 客户端（Streamable HTTP）

为什么需要它：`code_runtime --debug` 会懒启动 JZUiMcpBridge（窗口一出现就起，
端口 = 调试端口+1，端点 /mcp），提供 ui_screenshot / ui_invoke / ui_windows ...
脚手架就用它取"屏幕"（PNG）与做动作 —— 这条通道是现成的，不用新增 JS 绑定。

⚠ 只用 ui_screenshot / ui_invoke / ui_windows / ui_activate_window。
   get_ui_state 是**答案本身**（语义树），做视觉评测时禁止调用（见 prompt.md）。

命令行（便于手工排查）:
  python mcp.py --port 9223 --list
  python mcp.py --port 9223 --call ui_screenshot
  python mcp.py --port 9223 --invoke "#n3" click
  python mcp.py --port 9223 --invoke "#n3" click --visual true
"""
import argparse
import json
import sys
import time
import urllib.error
import urllib.request

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass


class McpClient:
    def __init__(self, port, host="127.0.0.1", path="/mcp", timeout=60):
        self.url = "http://%s:%d%s" % (host, port, path)
        self.timeout = timeout
        self._id = 0

    # ---- 传输 ----
    def _post(self, payload):
        data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        req = urllib.request.Request(self.url, data=data, method="POST")
        req.add_header("Content-Type", "application/json")
        req.add_header("Accept", "application/json, text/event-stream")
        with urllib.request.urlopen(req, timeout=self.timeout) as resp:
            raw = resp.read().decode("utf-8", "replace")
            ctype = resp.headers.get("Content-Type", "") or ""
        return _parse_body(raw, ctype)

    def rpc(self, method, params=None):
        self._id += 1
        msg = {"jsonrpc": "2.0", "id": self._id, "method": method}
        if params is not None:
            msg["params"] = params
        out = self._post(msg)
        if out is None:
            raise RuntimeError("空响应: %s" % method)
        if "error" in out:
            raise RuntimeError("MCP error: %s" % json.dumps(out["error"], ensure_ascii=False))
        return out.get("result")

    # ---- 协议 ----
    def initialize(self):
        """握手。失败不影响后续(有些实现不强制)。"""
        try:
            return self.rpc("initialize", {
                "protocolVersion": "2025-06-18",
                "capabilities": {},
                "clientInfo": {"name": "auto_code-scaffold", "version": "0.1.0"},
            })
        except Exception as e:
            print("[mcp] initialize 失败(继续): %s" % e)
            return None

    def list_tools(self):
        res = self.rpc("tools/list", {})
        return [t.get("name") for t in (res or {}).get("tools", [])]

    def call(self, name, args=None):
        """返回 (text, is_error)。"""
        res = self.rpc("tools/call", {"name": name, "arguments": args or {}})
        return _text_of(res)

    def call_or_raise(self, name, args=None):
        text, err = self.call(name, args)
        if err:
            raise RuntimeError("%s 失败: %s" % (name, text))
        return text

    # ---- 便捷封装 ----
    def screenshot(self):
        """抓当前目标窗口 -> PNG。返回工具给的文本(含保存路径)。"""
        return self.call_or_raise("ui_screenshot")

    def screenshot_path(self):
        """抓图并返回 PNG 文件路径（工具存到 exe 目录, 所以要把路径解析出来）。"""
        import re
        text = self.screenshot()
        m = re.search(r"screenshot saved:\s*(.+?)\s*(?:\(|$)", text)
        if not m:
            raise RuntimeError("没解析出截图路径: %s" % text)
        return m.group(1).strip()

    def windows(self):
        return self.call_or_raise("ui_windows")

    def invoke(self, target, action="click", value=None, visual=None):
        """驱动一个控件。visual 默认不传 = 用 bridge 的默认值(关)。返回 (text, is_error)。"""
        a = {"target": target, "action": action}
        if value is not None:
            a["value"] = str(value)
        if visual is not None:
            a["visual"] = bool(visual)
        return self.call("ui_invoke", a)

    def wait_ready(self, timeout=30, interval=0.4):
        """等 UI MCP 真的可用(场景进程懒启动, 窗口出现才起)。"""
        end = time.time() + timeout
        last = ""
        while time.time() < end:
            try:
                self.windows()
                return True
            except urllib.error.URLError as e:
                last = str(e)
            except Exception as e:
                last = str(e)
            time.sleep(interval)
        raise RuntimeError("等 UI MCP 就绪超时(%ss): %s" % (timeout, last))


def _text_of(res):
    """统一返回 (text, is_error) 二元组（工具结果异常时也不炸调用方）。"""
    if not isinstance(res, dict):
        return ("" if res is None else str(res)), False
    parts = []
    for item in res.get("content", []) or []:
        if isinstance(item, dict) and item.get("type") == "text":
            parts.append(item.get("text", ""))
    if not parts:
        return json.dumps(res, ensure_ascii=False)
    return "\n".join(parts), bool(res.get("isError", False))


def _parse_body(raw, ctype):
    """响应可能是纯 JSON, 也可能是 POST 内联 SSE(每块 data: {...})。"""
    raw = (raw or "").strip()
    if not raw:
        return None
    if raw.startswith("{") or raw.startswith("["):
        try:
            return json.loads(raw)
        except Exception:
            pass
    last = None
    for line in raw.splitlines():
        line = line.strip()
        if line.startswith("data:"):
            chunk = line[5:].strip()
            if not chunk:
                continue
            try:
                obj = json.loads(chunk)
            except Exception:
                continue
            if isinstance(obj, dict) and ("result" in obj or "error" in obj):
                last = obj
    return last


def main():
    ap = argparse.ArgumentParser(description="极简 MCP 客户端")
    ap.add_argument("--port", type=int, default=9223)
    ap.add_argument("--list", action="store_true", help="列出工具")
    ap.add_argument("--call", help="调用工具名")
    ap.add_argument("--args", default="{}", help="工具参数 JSON")
    ap.add_argument("--screenshot", action="store_true", help="抓一次图并打印 PNG 路径")
    ap.add_argument("--invoke", nargs=2, metavar=("TARGET", "ACTION"), help="ui_invoke 快捷方式")
    ap.add_argument("--visual", choices=["true", "false"], help="ui_invoke 是否显示 AI 光标/蒙版")
    args = ap.parse_args()

    c = McpClient(args.port)
    c.initialize()
    if args.list:
        print(c.list_tools())
        return 0
    if args.call:
        print(c.call(args.call, json.loads(args.args)))
        return 0
    if args.screenshot:
        p = c.screenshot_path()
        print(p)
        return 0
    if args.invoke:
        visual = None if args.visual is None else (args.visual == "true")
        t0 = time.time()
        print(c.invoke(args.invoke[0], args.invoke[1], visual=visual))
        print("[mcp] elapsed=%.0fms" % ((time.time() - t0) * 1000))
        return 0
    print("什么都没做 —— 用 --list / --call / --invoke")
    return 0


if __name__ == "__main__":
    sys.exit(main())
