#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""managed server 异步任务生命周期与过程事件回归。

覆盖 wait:false 的取消/超时、多队列 busy 聚合、managed reply 上行，以及
server timeout 终态不被迟到的 taskEnd 覆盖。
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_remote_ssh_nodes import (  # noqa: E402
    free_port, fwd, http_json, kill_all, log, start_proc, wait_tcp, write_json,
)


class ControlledLlm(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    outside_path = ""
    outside_secret = ""
    outside_command = ""
    outside_used = False

    def log_message(self, fmt, *args):
        pass

    def do_POST(self):  # noqa: N802
        n = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(n) if n else b""
        time.sleep(1.5)  # 给取消与 1s server 超时留出确定窗口

        content = "managed reply"
        finish = None
        delta: dict = {"content": content}
        if self.outside_path and b"PERMISSION-CASE" in raw:
            if not self.outside_used:
                type(self).outside_used = True
                if self.outside_command:
                    tool_name = "terminal_run"
                    tool_args = {"command": self.outside_command}
                else:
                    tool_name = "read_file"
                    tool_args = {"path": self.outside_path}
                delta = {"tool_calls": [{
                    "index": 0, "id": "permission_case", "type": "function",
                    "function": {"name": tool_name, "arguments": json.dumps(tool_args)},
                }]}
                finish = "tool_calls"
            else:
                content = ("OUTSIDE_READ_OK" if self.outside_secret.encode("utf-8") in raw
                           else "OUTSIDE_DONE")
                delta = {"content": content}
        chunk = {
            "id": "mock", "object": "chat.completion.chunk", "created": 0, "model": "mock",
            "choices": [{"index": 0, "delta": delta, "finish_reason": finish}],
            "usage": {"prompt_tokens": 5, "completion_tokens": 5, "total_tokens": 10},
        }
        final = {
            "id": "mock", "object": "chat.completion.chunk", "created": 0, "model": "mock",
            "choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}],
        }
        head = ("data: " + json.dumps(chunk) + "\n\n").encode("utf-8")
        tail = ("data: " + json.dumps(final) + "\n\n" + "data: [DONE]\n\n").encode("utf-8")
        def write_chunk(data: bytes) -> None:
            self.wfile.write(("%x\r\n" % len(data)).encode("ascii") + data + b"\r\n")
            self.wfile.flush()

        try:
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream; charset=utf-8")
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            write_chunk(head)            # reply 事件先上行
            time.sleep(1.0)              # taskEnd 后到, 测试可观察中间状态
            write_chunk(tail)
            self.wfile.write(b"0\r\n\r\n")
            self.wfile.flush()
        except OSError:
            pass                         # 对端取消是测试的一部分


def wait_online(port: int, agent_id: str, timeout: float = 20.0) -> dict:
    deadline = time.time() + timeout
    last = {}
    while time.time() < deadline:
        try:
            st = http_json("http://127.0.0.1:%d/api/status" % port, None, 2.0)
            last = st
            for a in st.get("online") or []:
                if a.get("agentId") == agent_id:
                    return a
        except Exception:                 # noqa: BLE001
            pass
        time.sleep(0.1)
    raise AssertionError("cli 未上线: " + json.dumps(last, ensure_ascii=False))


def detail(port: int, task_id: str) -> dict:
    return http_json("http://127.0.0.1:%d/api/tasks/detail?id=%s" % (port, task_id), None, 2.0)


def wait_status(port: int, task_id: str, wanted: set, timeout: float = 8.0) -> dict:
    deadline = time.time() + timeout
    last = {}
    while time.time() < deadline:
        last = detail(port, task_id)
        if last.get("status") in wanted:
            return last
        time.sleep(0.1)
    raise AssertionError("任务状态未达到 %s: %s" % (wanted, last))


def submit_async(port: int, prompt: str, timeout_ms: int = 15000, agent_id: str = "a1") -> str:
    r = http_json("http://127.0.0.1:%d/api/tasks" % port, {
        "agentId": agent_id, "prompt": prompt, "wait": False,
        "use_memory": False, "task_timeout_ms": timeout_ms,
    }, 3.0)
    assert r.get("status") == "running" and r.get("taskId"), r
    return r["taskId"]


def cancel(port: int, task_id: str) -> None:
    r = http_json("http://127.0.0.1:%d/api/tasks/cancel?taskId=%s" % (port, task_id), {}, 3.0)
    assert r.get("ok") is True, r


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--agent-cli", required=True)
    ap.add_argument("--agent-server", required=True)
    ap.add_argument("--tmp", default="")
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args(argv)
    agent_cli = os.path.abspath(args.agent_cli)
    agent_server = os.path.abspath(args.agent_server)
    tmp = os.path.abspath(args.tmp) if args.tmp else tempfile.mkdtemp(prefix="jz_server_tasks_")
    ws = os.path.join(tmp, "ws")
    logs = os.path.join(tmp, "logs")
    os.makedirs(ws, exist_ok=True)
    os.makedirs(logs, exist_ok=True)

    llm = ThreadingHTTPServer(("127.0.0.1", 0), ControlledLlm)
    threading.Thread(target=llm.serve_forever, daemon=True).start()
    model_url = "http://127.0.0.1:%d/v1/chat/completions" % llm.server_address[1]
    port_http, port_hub = free_port(), free_port()
    token = "hub-test"
    cfg_cli = os.path.join(tmp, "cli.json")
    cfg_server = os.path.join(tmp, "server.json")
    write_json(cfg_cli, {
        "agentName": "agent_cli", "logFile": fwd(os.path.join(logs, "cli.log")),
        "llm": {"url": model_url, "name": "mock", "maxRounds": 10,
                "maxRetries": 0, "maxFixRetries": 0, "netTimeoutMs": 10000},
        "task": {"queues": 2, "maxPending": 8},
        "terminal": {"rootDir": fwd(ws), "allowTerminal": True, "allowFileTools": True},
        "network": {"enabled": False},
        "server": {"host": "127.0.0.1", "port": port_hub, "token": token, "agentId": "a1"},
    })
    write_json(cfg_server, {
        "http": {"bind": "127.0.0.1", "port": port_http, "token": ""},
        "hub": {"bind": "127.0.0.1", "port": port_hub, "token": token},
        "frp": {"server": {"enabled": False}}, "agents": [],
        "selfAgent": {"enabled": True, "agentId": "server", "rootDir": fwd(ws),
                      "modelUrl": model_url, "modelName": "mock"},
    })

    procs = []
    try:
        procs.append(start_proc(agent_server, cfg_server, os.path.join(logs, "server.out.txt"),
                                os.path.dirname(agent_server)))
        assert wait_tcp(port_http, 20) and wait_tcp(port_hub, 20)
        procs.append(start_proc(agent_cli, cfg_cli, os.path.join(logs, "cli.out.txt"),
                                os.path.dirname(agent_cli)))
        wait_online(port_http, "a1")

        # A/B 在跑, C 排队。只取消 A 后, C 必须等 A 完整收尾再启动, 不能被 reset 误清。
        a = submit_async(port_http, "TASK-A")
        b = submit_async(port_http, "TASK-B")
        c_running = submit_async(port_http, "TASK-C-RUNNING")
        cancel(port_http, a)
        assert wait_status(port_http, a, {"cancelled"}).get("status") == "cancelled"
        deadline = time.time() + 4
        while time.time() < deadline:
            c_info = detail(port_http, c_running)
            if c_info.get("last_event") == "task_start":
                break
            time.sleep(0.05)
        assert c_info.get("last_event") == "task_start", c_info

        # D 仍在排队时取消, 不能影响 B/C; 随后再取消正在运行的 C。
        queued = submit_async(port_http, "TASK-QUEUED")
        cancel(port_http, queued)
        assert wait_status(port_http, queued, {"cancelled"}).get("status") == "cancelled"
        assert detail(port_http, b).get("status") == "running"
        cancel(port_http, c_running)
        wait_status(port_http, c_running, {"cancelled"})
        assert detail(port_http, b).get("status") == "running"

        # B 独立在跑时控制面仍必须 busy。
        deadline = time.time() + 3
        while time.time() < deadline:
            online = wait_online(port_http, "a1")
            if online.get("busy"):
                break
            time.sleep(0.05)
        assert online.get("busy") is True, online
        cancel(port_http, b)
        wait_status(port_http, b, {"cancelled"})
        log("wait:false 排队/在跑取消无重入; 其他队列仍保持 busy")

        # 异步任务也有 server 超时; 迟到 cancelled 不得覆盖 timeout 的 failed。
        c = submit_async(port_http, "TASK-C", timeout_ms=1000)
        timeout_rec = wait_status(port_http, c, {"failed"}, timeout=5)
        assert "超时" in timeout_rec.get("error", ""), timeout_rec
        time.sleep(2.2)
        assert detail(port_http, c).get("status") == "failed", detail(port_http, c)
        log("wait:false 有 server 超时; 迟到 taskEnd 不覆盖终态")

        # selfAgent.stop() 会同步发 done; 超时时也必须先落 failed, 不能变成 cancelled。
        self_task = submit_async(port_http, "SELF-TIMEOUT", timeout_ms=1000, agent_id="server")
        self_rec = wait_status(port_http, self_task, {"failed"}, timeout=5)
        assert "超时" in self_rec.get("error", ""), self_rec
        time.sleep(2.2)
        assert detail(port_http, self_task).get("status") == "failed", detail(port_http, self_task)
        log("selfAgent 超时终态保持 failed")

        # 相对路径必须按 selfAgent.rootDir 解析(区外只读本来就是允许的)。
        inside = os.path.join(ws, "inside.txt")
        with open(inside, "w", encoding="utf-8") as f:
            f.write("INSIDE-SECRET")
        ControlledLlm.outside_path = "inside.txt"
        ControlledLlm.outside_secret = "INSIDE-SECRET"
        ControlledLlm.outside_command = ""
        ControlledLlm.outside_used = False
        self_read = submit_async(port_http, "PERMISSION-CASE", agent_id="server")
        self_read_rec = wait_status(port_http, self_read, {"success", "failed"}, timeout=10)
        assert self_read_rec.get("status") == "success", self_read_rec
        assert self_read_rec.get("agent_reply") == "OUTSIDE_READ_OK", self_read_rec

        # 终端的区域外写入必须被拒绝; 不能选系统临时目录(那里本来就允许读写)。
        escape = os.path.abspath("build/_self_agent_escape_%d.txt" % os.getpid())
        if os.path.isfile(escape):
            os.remove(escape)
        ControlledLlm.outside_path = "enabled"
        ControlledLlm.outside_secret = ""
        ControlledLlm.outside_command = 'echo bad > "%s"' % fwd(escape)
        ControlledLlm.outside_used = False
        self_term = submit_async(port_http, "PERMISSION-CASE", agent_id="server")
        self_term_rec = wait_status(port_http, self_term, {"success", "failed"}, timeout=10)
        assert self_term_rec.get("status") == "success", self_term_rec
        escaped = os.path.isfile(escape)
        if escaped:
            os.remove(escape)
        assert not escaped, "selfAgent 终端写出了工作区"
        ControlledLlm.outside_path = ""
        ControlledLlm.outside_command = ""
        log("selfAgent 相对路径解析与区外写入限制正常")

        # managed 没有本地 SSE, 但 reply 仍必须经 hub 上行。
        d = submit_async(port_http, "TASK-D")
        deadline = time.time() + 6
        seen_reply = False
        while time.time() < deadline:
            online = wait_online(port_http, "a1")
            if online.get("lastEventType") == "reply":
                seen_reply = True
                break
            time.sleep(0.05)
        assert seen_reply, online
        assert wait_status(port_http, d, {"success"}).get("status") == "success"
        log("managed reply 事件上行正常")

        log("PASS: server 异步任务生命周期与事件回归")
        return 0
    except Exception as e:
        log("FAIL: %r" % (e,))
        return 1
    finally:
        kill_all(procs)
        llm.shutdown()
        if not args.keep:
            import shutil
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
