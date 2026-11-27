#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""复现长任务事件超过列表页上限后, 旧 renderTask 签名不再变化。

fake LLM 连续发 terminal_run, 让任务在 WORKING 态持续产生事件。
server 的 /api/tasks 每任务只回最近 30 条事件, 旧前端签名只看 events.length;
超过 30 条后 length 卡在 30, 即使 eventSeq 继续增长也不会重绘。
本用例同时断言: 加上 eventSeq 的新签名会变化。
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
from test_server_tasks import (  # noqa: E402
    artifact_text, cancel, state, submit_async, wait_online, wait_status,
)

TOOL_ROUNDS = 60
WORKING = {"TASK_STATE_SUBMITTED", "TASK_STATE_WORKING"}


class ManyEventsLlm(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    hits = 0
    saw_big_ctx = False

    def log_message(self, fmt, *args):
        pass

    def do_POST(self):  # noqa: N802
        n = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(n) if n else b""
        if b"BIGCTX-0-" in raw:
            type(self).saw_big_ctx = True
        i = type(self).hits
        type(self).hits += 1
        time.sleep(0.12)

        if i < TOOL_ROUNDS:
            delta = {"tool_calls": [{
                "index": 0, "id": "tick-%d" % i, "type": "function",
                "function": {"name": "terminal_run",
                             "arguments": json.dumps({"command": "echo tick-%d" % i})},
            }]}
            finish = "tool_calls"
        else:
            # 大回复同时覆盖 taskEvent/taskEnd 的 zlib 往返(>4KB 才会压)。
            delta = {"content": "done-" + ("Z" * 9000)}
            finish = None

        chunk = {
            "id": "mock", "object": "chat.completion.chunk", "created": 0, "model": "mock",
            "choices": [{"index": 0, "delta": delta, "finish_reason": finish}],
            "usage": {"prompt_tokens": 5, "completion_tokens": 5, "total_tokens": 10},
        }
        tail = {
            "id": "mock", "object": "chat.completion.chunk", "created": 0, "model": "mock",
            "choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}],
        }
        head_data = ("data: " + json.dumps(chunk) + "\n\n").encode("utf-8")
        tail_data = ("data: " + json.dumps(tail) + "\n\n"
                     + "data: [DONE]\n\n").encode("utf-8")

        def write_chunk(data: bytes) -> None:
            self.wfile.write(("%x\r\n" % len(data)).encode("ascii") + data + b"\r\n")
            self.wfile.flush()

        try:
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream; charset=utf-8")
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            write_chunk(head_data)
            write_chunk(tail_data)
            self.wfile.write(b"0\r\n\r\n")
            self.wfile.flush()
        except OSError:
            pass


def old_sig(t: dict) -> tuple:
    m = t.get("metadata") or {}
    return (t.get("id"), state(t), len(m.get("events") or []), len(artifact_text(t)))


def new_sig(t: dict) -> tuple:
    m = t.get("metadata") or {}
    return old_sig(t) + (int(m.get("eventSeq") or 0),)


def get_task(port: int, task_id: str) -> dict:
    all_tasks = http_json(
        "http://127.0.0.1:%d/api/tasks?agentId=a1&origin=user&limit=50" % port, None, 2.0)
    for t in all_tasks.get("tasks") or []:
        if t.get("id") == task_id:
            return t
    raise AssertionError("任务不在列表里: " + task_id)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--agent-cli", required=True)
    ap.add_argument("--agent-server", required=True)
    args = ap.parse_args(argv)
    agent_cli = os.path.abspath(args.agent_cli)
    agent_server = os.path.abspath(args.agent_server)

    tmp = tempfile.mkdtemp(prefix="jz_event_refresh_")
    ws, logs = os.path.join(tmp, "ws"), os.path.join(tmp, "logs")
    os.makedirs(ws, exist_ok=True)
    os.makedirs(logs, exist_ok=True)

    llm = ThreadingHTTPServer(("127.0.0.1", 0), ManyEventsLlm)
    threading.Thread(target=llm.serve_forever, daemon=True).start()
    model_url = "http://127.0.0.1:%d/v1/chat/completions" % llm.server_address[1]
    port_http, port_hub = free_port(), free_port()
    token = "event-refresh-test"

    cfg_cli = os.path.join(tmp, "cli.json")
    cfg_server = os.path.join(tmp, "server.json")
    write_json(cfg_cli, {
        "agentName": "agent_cli", "logFile": fwd(os.path.join(logs, "cli.log")),
        "llm": {"models": [{"name": "mock", "url": model_url, "contextWindow": 160000}],
                "maxRounds": TOOL_ROUNDS + 4, "maxRetries": 0, "maxFixRetries": 0,
                "netTimeoutMs": 30000},
        "task": {"queues": 1, "maxPending": 4},
        "terminal": {"rootDir": fwd(ws), "allowTerminal": True, "allowFileTools": True},
        "network": {"enabled": False},
        "server": {"host": "127.0.0.1", "port": port_hub, "token": token, "agentId": "a1"},
    })
    write_json(cfg_server, {
        "http": {"bind": "127.0.0.1", "port": port_http, "ssl": {"enabled": False}},
        "hub": {"bind": "127.0.0.1", "port": port_hub, "token": token},
        "agents": [],
    })

    procs = []
    task_id = ""
    try:
        procs.append(start_proc(agent_server, cfg_server, os.path.join(logs, "server.out.txt"),
                                os.path.dirname(agent_server)))
        assert wait_tcp(port_http, 20) and wait_tcp(port_hub, 20)
        procs.append(start_proc(agent_cli, cfg_cli, os.path.join(logs, "cli.out.txt"),
                                os.path.dirname(agent_cli)))
        wait_online(port_http, "a1")
        task_id = submit_async(port_http, "EVENT-REFRESH", timeout_ms=60000)

        # 等列表页被截到 30 条(eventsOffset > 0)且任务仍 WORKING。
        deadline = time.time() + 20
        first = None
        last = None
        while time.time() < deadline:
            t = get_task(port_http, task_id)
            m = t.get("metadata") or {}
            last = t
            if (state(t) in WORKING and len(m.get("events") or []) >= 30
                    and int(m.get("eventsOffset") or 0) > 0):
                first = t
                break
            time.sleep(0.05)
        assert first is not None, "没有进入 30 条封顶的 WORKING 态: %s" % (
            json.dumps((last or {}).get("metadata"), ensure_ascii=False))
        m1 = first["metadata"]
        log("封顶快照: seq=%s total=%s offset=%s" %
            (m1.get("eventSeq"), m1.get("eventsTotal"), m1.get("eventsOffset")))

        # 继续等 eventSeq 增长; 旧签名必须不动, 新签名必须变化。
        deadline = time.time() + 15
        second = None
        while time.time() < deadline:
            t = get_task(port_http, task_id)
            m = t.get("metadata") or {}
            if state(t) not in WORKING:
                break
            if int(m.get("eventSeq") or 0) > int(m1.get("eventSeq") or 0):
                second = t
                break
            time.sleep(0.05)
        assert second is not None, "eventSeq 没有继续增长"
        assert old_sig(second) == old_sig(first), (
            "旧签名不应变化: %s -> %s" % (old_sig(first), old_sig(second)))
        assert new_sig(second) != new_sig(first), (
            "新签名必须变化: %s -> %s" % (new_sig(first), new_sig(second)))
        log("复现成功: 旧签名不变 %s, 新签名 %s -> %s" %
            (old_sig(first), new_sig(first), new_sig(second)))

        # /api/tasks/events?after=<seq> 必须只回新事件, 供聊天区增量刷新。
        inc = http_json("http://127.0.0.1:%d/api/tasks/events?id=%s&after=%s&limit=500" %
                        (port_http, task_id, m1.get("eventSeq")), None, 3.0)
        seqs = [int(e.get("seq") or 0) for e in inc.get("events") or []]
        assert seqs and min(seqs) > int(m1.get("eventSeq") or 0), seqs
        log("after 增量接口: %d 条, seq %s..%s" % (len(seqs), min(seqs), max(seqs)))

        # context_history 增量必须已落到 ctx 文件, 且 assistant(tool_calls)/tool 成对。
        ctx_path = os.path.join(os.path.dirname(agent_server), "data", "ctx",
                                "ctx-" + task_id + ".json")
        deadline = time.time() + 3
        roles = []
        while time.time() < deadline:
            if os.path.exists(ctx_path):
                with open(ctx_path, "r", encoding="utf-8") as f:
                    roles = [h.get("role") for h in (json.load(f).get("history") or [])]
                if len(roles) >= 5:
                    break
            time.sleep(0.05)
        assert roles and roles[0] == "user", roles
        for i in range(1, len(roles) - 1, 2):
            assert roles[i] == "assistant" and roles[i + 1] == "tool", roles
        log("增量上下文已落盘: %d 条历史, assistant/tool 成对" % len(roles))

        # 大上下文「继续」: ctx 约 1.45MB, 远低于新的 10MB 单包上限。server 走 zlib 后应完整送达,
        # 不能 FATAL abort, 也不能靠裁剪丢掉最老一轮(用 fake LLM 收到的 BIGCTX-0- 验证)。
        # 假 LLM 会一直复读, 最后被循环检测中止 -> 历史结束在 error, 终态是 FAILED;
        # 这里只关心"任务跑完了且大回复完整送达"。
        done = wait_status(port_http, task_id, {"TASK_STATE_COMPLETED", "TASK_STATE_FAILED"}, 20.0)
        assert "Z" * 1000 in artifact_text(done), "大回复经 taskEvent/taskEnd 压缩后丢失"
        big = "x" * 9000
        big_history = []
        for i in range(80):
            big_history.append({"role": "user", "kind": "message",
                                "content": "BIGCTX-%d-%s" % (i, big)})
            big_history.append({"role": "assistant", "kind": "message",
                                "content": "reply-%d-%s" % (i, big)})
        with open(ctx_path, "w", encoding="utf-8") as f:
            json.dump({"history": big_history, "usage": {}}, f)
        ManyEventsLlm.saw_big_ctx = False
        cont = http_json("http://127.0.0.1:%d/api/tasks" % port_http, {
            "agentId": "a1",
            "taskId": task_id,
            "message": {"role": "ROLE_USER", "parts": [{"text": "CONTINUE-BIG-CTX"}]},
            "returnImmediately": True,
            "taskTimeoutMs": 30000,
        }, 3.0)
        assert cont.get("id") == task_id and state(cont) in WORKING, cont
        wait_status(port_http, task_id, {"TASK_STATE_COMPLETED", "TASK_STATE_FAILED"}, 20.0)
        assert ManyEventsLlm.saw_big_ctx, "大 ctx 没有完整送达模型(压缩/续聊断链)"
        log("大 ctx 继续: 压缩后完整送达, fake LLM 看到 BIGCTX-0-")
        return 0
    finally:
        if task_id:
            try:
                cancel(port_http, task_id)
            except Exception:  # noqa: BLE001 - 清理阶段不掩盖主断言
                pass
        kill_all(procs)
        llm.shutdown()


if __name__ == "__main__":
    sys.exit(main())
