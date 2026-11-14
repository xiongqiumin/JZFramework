#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""A2A 形状的管理台全链路自检：假 LLM → server → cli → 网页/API/SSE。

一条命令跑通(默认全是本机随机端口, 不联网、不备证书):
    python binary/agent_cli/test/test_a2a_flow.py \
        --agent-cli <build>/binary/bin/Debug/agent_cli.exe \
        --agent-server <build>/binary/bin/Debug/agent_cli_server.exe

想自己打开管理台看界面(保持进程不退出):
    python ... --serve
    # 打印出 http://127.0.0.1:<port>/ 后用浏览器打开, 点「下发指令」

验证: SendMessage(202 Task) → AgentCard → SubscribeToTask(SSE 首帧/终态帧) →
GetTask(TaskState / history / artifacts / events) → ListTasks(contextId 过滤) →
三个静态页。假 LLM 一律回「任务已成功」, 所以终态必须是 success。
"""
from __future__ import annotations

import argparse
import http.client
import json
import os
import socket
import sys
import tempfile
import threading
import time
import urllib.request
import uuid
from urllib.parse import urlparse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fake_llm import DEFAULT_REPLY, start_fake_llm  # noqa: E402
from test_remote_ssh_nodes import (  # noqa: E402
    free_port, fwd, http_json, kill_all, log, start_proc, wait_tcp, write_json,
)

WORKING_STATES = {
    "TASK_STATE_SUBMITTED", "TASK_STATE_WORKING",
    "TASK_STATE_INPUT_REQUIRED", "TASK_STATE_AUTH_REQUIRED",
}


def get_text(url: str, timeout: float = 3.0) -> str:
    with urllib.request.urlopen(url, timeout=timeout) as resp:
        return resp.read().decode("utf-8", "replace")


def detail(port: int, task_id: str) -> dict:
    return http_json("http://127.0.0.1:%d/api/tasks/detail?id=%s" % (port, task_id), None, 3.0)


def wait_terminal(port: int, task_id: str, timeout: float = 20.0) -> dict:
    deadline = time.time() + timeout
    last = {}
    while time.time() < deadline:
        last = detail(port, task_id)
        if last.get("status", {}).get("state") not in WORKING_STATES:
            return last
        time.sleep(0.1)
    raise AssertionError("任务没到终态: " + json.dumps(last, ensure_ascii=False))


def wait_online(port: int, agent_id: str, timeout: float = 20.0) -> None:
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            st = http_json("http://127.0.0.1:%d/api/status" % port, None, 2.0)
            if any(a.get("agentId") == agent_id for a in (st.get("online") or [])):
                return
        except Exception:  # noqa: BLE001
            pass
        time.sleep(0.1)
    raise AssertionError("cli 未上线: " + agent_id)


def read_sse(url: str, timeout: float = 8.0) -> list:
    """逐行读 SSE(chunked 由 http.client 解码)，收集完整帧。"""
    u = urlparse(url)
    conn = http.client.HTTPConnection(u.hostname, u.port, timeout=2.0)
    conn.request("GET", u.path + (("?" + u.query) if u.query else ""))
    resp = conn.getresponse()
    if resp.status != 200:
        raise AssertionError("SSE HTTP %d" % resp.status)
    frames, buf = [], []
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            line = resp.readline()
        except socket.timeout:
            break
        if not line:
            break
        s = line.decode("utf-8", "replace").rstrip("\r\n")
        if s == "":
            if buf:
                ev = {}
                for item in buf:
                    if item.startswith("event: "):
                        ev["event"] = item[7:]
                    elif item.startswith("data: "):
                        ev["data"] = item[6:]
                frames.append(ev)
                buf = []
                if ev.get("event") == "task":
                    try:
                        st = json.loads(ev.get("data", "{}")).get("status", {})
                        if isinstance(st, dict) and st.get("state") not in WORKING_STATES:
                            break   # 终态帧: 后端随后关流
                    except Exception:  # noqa: BLE001
                        pass
            continue
        if s.startswith(":"):
            continue        # keepalive 注释
        buf.append(s)
    conn.close()
    return frames


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--agent-cli", required=True)
    ap.add_argument("--agent-server", required=True)
    ap.add_argument("--tmp", default="")
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--serve", action="store_true", help="只起服务、不自动提交, 供手动开网页")
    ap.add_argument("--llm-delay", type=float, default=0.3, help="假 LLM 的人为延迟(秒)")
    args = ap.parse_args(argv)

    agent_cli = os.path.abspath(args.agent_cli)
    agent_server = os.path.abspath(args.agent_server)
    tmp = os.path.abspath(args.tmp) if args.tmp else tempfile.mkdtemp(prefix="jz_a2a_flow_")
    ws, logs_dir = os.path.join(tmp, "ws"), os.path.join(tmp, "logs")
    os.makedirs(ws, exist_ok=True)
    os.makedirs(logs_dir, exist_ok=True)

    log("临时目录: " + tmp)
    llm, model_url = start_fake_llm(delay=args.llm_delay)
    port_http, port_hub = free_port(), free_port()
    token = "a2a-flow"
    cfg_cli, cfg_server = os.path.join(tmp, "cli.json"), os.path.join(tmp, "server.json")
    write_json(cfg_cli, {
        "agentName": "agent_cli", "logFile": fwd(os.path.join(logs_dir, "cli.log")),
        "llm": {"url": model_url, "name": "fake", "maxRounds": 10,
                "maxRetries": 0, "maxFixRetries": 0, "netTimeoutMs": 10000},
        "task": {"queues": 1, "maxPending": 4},
        "terminal": {"rootDir": fwd(ws), "allowTerminal": True, "allowFileTools": True},
        "network": {"enabled": False},
        "server": {"host": "127.0.0.1", "port": port_hub, "token": token, "agentId": "a1"},
    })
    write_json(cfg_server, {
        # 本机回归: 明文 HTTP 只绑回环, 不对外; 生产默认仍是 HTTPS(单向)
        "http": {"bind": "127.0.0.1", "port": port_http, "ssl": {"enabled": False}},
        "hub": {"bind": "127.0.0.1", "port": port_hub, "token": token},
        "agents": [],
        "selfAgent": {"enabled": True, "agentId": "server", "rootDir": fwd(ws),
                      "modelUrl": model_url, "modelName": "fake"},
    })

    base = "http://127.0.0.1:%d" % port_http
    procs = []
    try:
        procs.append(start_proc(agent_server, cfg_server,
                                os.path.join(logs_dir, "server.out.txt"),
                                os.path.dirname(agent_server)))
        assert wait_tcp(port_http, 20) and wait_tcp(port_hub, 20), "server 端口没起来"
        procs.append(start_proc(agent_cli, cfg_cli,
                                os.path.join(logs_dir, "cli.out.txt"),
                                os.path.dirname(agent_cli)))
        wait_online(port_http, "a1")
        log("链路已通: fake_llm -> server(8088 类) -> cli(managed) -> 管理台")

        # 静态页(POST_BUILD 拷过去的 server_web/)
        for path, keyword in (("/", "Agent 台账"), ("/tasks", "Task 详情"),
                              ("/new", "下发指令"), ("/agent", "AgentCard")):
            html = get_text(base + path)
            assert keyword in html, "%s 页面内容不对" % path
        assert "A2A" in get_text(base + "/app.js"), "app.js 不是新版本(清缓存/重建)"
        log("管理台 4 个页面 + app.js 已发布")

        # AgentCard
        card = http_json(base + "/api/agent/card?id=a1", None, 3.0)
        assert card["capabilities"]["streaming"] is True, card
        assert any(s["id"] == "task" for s in card["skills"]), card
        assert card["metadata"]["online"] is True, card
        log("AgentCard: skills/capabilities/metadata/online 齐全")

        if args.serve:
            log("管理台: %s/   (明文仅回环, 生产用 HTTPS)" % base)
            log("按 Ctrl+C 结束")
            try:
                while True:
                    time.sleep(1)
            except KeyboardInterrupt:
                log("结束")

        # SendMessage(returnImmediately:true) -> 202 一个 A2A Task
        # contextId 每次唯一: server 的 data/tasks.jsonl 在 exe 目录里是跨运行累积的,
        # 用固定 ID 会让 ListTasks 的计数被上一次自检污染。
        context_id = "ctx-a2a-demo-" + uuid.uuid4().hex[:8]
        ack = http_json(base + "/api/tasks", {
            "agentId": "a1",
            "message": {"role": "ROLE_USER", "parts": [{"text": "跑一遍假 LLM 链路"}]},
            "returnImmediately": True,
            "contextId": context_id,
        }, 5.0)
        task_id = ack["id"]
        assert ack["status"]["state"] in WORKING_STATES, ack
        assert ack["contextId"] == context_id, ack
        log("SendMessage -> %s (%s, contextId=%s)" %
            (task_id, ack["status"]["state"], ack["contextId"]))

        # SubscribeToTask: 提交后立刻订阅(SSE), 后台收帧
        frames = []
        def subscribe():
            try:
                frames.extend(read_sse(base + "/api/tasks/subscribe?id=" + task_id))
            except Exception as e:  # noqa: BLE001
                frames.append({"event": "error", "data": str(e)})
        th = threading.Thread(target=subscribe, daemon=True)
        th.start()

        final = wait_terminal(port_http, task_id)
        th.join(timeout=10)
        assert not any(f.get("event") == "error" for f in frames), frames
        assert any(f.get("event") == "task" for f in frames), "SSE 没收到首帧/终态 Task 快照: %s" % frames
        log("SubscribeToTask: 收到 %d 帧(首帧 Task 快照 + 状态/产物更新)" % len(frames))

        # GetTask: 只有 A2A Task 一种形状
        assert final["status"]["state"] == "TASK_STATE_COMPLETED", final
        assert final["contextId"] == context_id, final
        assert final["history"][0]["role"] == "ROLE_USER", final
        assert any(a["artifactId"] == "reply" and a["parts"][0]["text"] == DEFAULT_REPLY
                   for a in final["artifacts"]), final
        assert final["metadata"]["agentId"] == "a1", final
        assert final["metadata"]["prompt"] == "跑一遍假 LLM 链路", final
        types = [e["type"] for e in final["metadata"]["events"]]
        assert "submitted" in types and "task_end" in types, types
        log("GetTask: state=COMPLETED, history/artifacts/metadata.events 齐全(%d 条事件)" % len(types))

        by_ctx = http_json(base + "/api/tasks?contextId=" + context_id, None, 3.0)
        assert len(by_ctx["tasks"]) == 1 and by_ctx["tasks"][0]["id"] == task_id, by_ctx
        assert by_ctx.get("totalSize", 0) > 0 and "nextPageToken" in by_ctx, by_ctx
        log("ListTasks: contextId 过滤生效")

        log("OK: 假 LLM「%s」→ Task 成功, 管理台全链路通过" % DEFAULT_REPLY)
        return 0
    finally:
        if not args.keep:
            kill_all(procs)
        llm.shutdown()


if __name__ == "__main__":
    raise SystemExit(main())
