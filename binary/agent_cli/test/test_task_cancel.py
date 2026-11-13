#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""standalone 任务池取消/reset 回归。

覆盖:
  1. queues=1 时, 在跑任务与排队任务 reset 后都必须回 cancelled;
  2. queues=2 时同样成立, 且取消不能漏掉任一在跑任务 / 排队任务;
  3. 两个并发任务对象的 reply 事件都到 master 的 SSE(2026-11-11 起本地事件统一 queue=0,
     没有常驻 q0/q1 下标), /info.recent 在**运行期间**同时汇总两条;
  4. 远端命令启动后 reset, node 进程必须真停, 不能只留下 cancelled 回包。
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import tempfile
import threading
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_remote_ssh_nodes import (  # noqa: E402
    MockLlm, free_port, fwd, http_json, kill_all, log, start_proc, wait_tcp, write_json,
)
from test_task_queues import SlowLlm  # noqa: E402
from http.server import ThreadingHTTPServer  # noqa: E402


class QuietSlowLlm(SlowLlm):
    def do_POST(self):  # 取消用例会主动断开 LLM 连接, 不让预期断连刷异常日志
        try:
            super().do_POST()
        except OSError:
            pass


class QuietMockLlm(MockLlm):
    def do_POST(self):
        try:
            super().do_POST()
        except OSError:
            pass


def submit(url: str, prompt: str, out: dict, key: str) -> None:
    try:
        out[key] = http_json(url, {"prompt": prompt, "use_memory": False}, 15.0)
    except Exception as e:                    # noqa: BLE001
        out[key] = {"error": repr(e)}


def read_sse(port: int, events: list, holder: dict) -> None:
    try:
        resp = urllib.request.urlopen("http://127.0.0.1:%d/events" % port, timeout=10)
        holder["resp"] = resp
        for raw in resp:
            if not raw.startswith(b"data: "):
                continue
            events.append(json.loads(raw[6:].decode("utf-8")))
    except Exception:                         # 主线程关闭响应即结束
        pass


def wait_pending(port: int, pending: int, timeout: float = 5.0) -> dict:
    deadline = time.time() + timeout
    last = {}
    while time.time() < deadline:
        last = http_json("http://127.0.0.1:%d/info" % port, None, 2.0)
        if last.get("pendingTasks", 0) >= pending:
            return last
        time.sleep(0.05)
    raise AssertionError("排队状态未达到预期: " + json.dumps(last, ensure_ascii=False))


def run_case(agent_cli: str, queues: int, root: str) -> None:
    ws = os.path.join(root, "ws")
    logs = os.path.join(root, "logs")
    os.makedirs(ws, exist_ok=True)
    os.makedirs(logs, exist_ok=True)

    llm = ThreadingHTTPServer(("127.0.0.1", 0), QuietSlowLlm)
    threading.Thread(target=llm.serve_forever, daemon=True).start()
    model_url = "http://127.0.0.1:%d/v1/chat/completions" % llm.server_address[1]
    port = free_port()
    cfg = os.path.join(root, "cli.json")
    write_json(cfg, {
        "agentName": "agent_cli",
        "logFile": fwd(os.path.join(logs, "agent_cli.log")),
        "llm": {"url": model_url, "name": "mock", "token": "",
                "maxRounds": 10, "netTimeoutMs": 60000},
        "verify": {"maxRounds": 1, "timeoutMs": 30000},
        "task": {"queues": queues, "maxPending": 8},
        "terminal": {"rootDir": fwd(ws), "allowTerminal": True, "allowFileTools": True},
        "network": {"enabled": False},
        "http": {"bind": "127.0.0.1", "port": port, "token": ""},
    })

    procs = []
    sse_holder: dict = {}
    try:
        procs.append(start_proc(agent_cli, cfg, os.path.join(logs, "cli.out.txt"),
                                os.path.dirname(agent_cli)))
        if not wait_tcp(port, 20):
            raise AssertionError("agent_cli 未启动")
        url = "http://127.0.0.1:%d/task" % port

        # 两个活动任务并发跑完: 验证事件都到 master SSE、recent 同时看得到两条。
        # ⚠ 2026-11-11 起任务对象按需新建、回包即销毁, 没有常驻的 q0/q1 编号:
        #   本地事件统一 queue=0, /info.queues 是"当前活着的任务对象"数组, recent 也只汇总
        #   还活着的任务 —— 所以 busy / recent 必须在**运行期间**查, 不能在 join 之后查。
        if queues == 2:
            events: list = []
            th_sse = threading.Thread(target=read_sse, args=(port, events, sse_holder), daemon=True)
            th_sse.start()
            out: dict = {}
            threads = [threading.Thread(target=submit, args=(url, "TASK-A", out, "a")),
                       threading.Thread(target=submit, args=(url, "TASK-B", out, "b"))]
            for th in threads:
                th.start()

            info_url = "http://127.0.0.1:%d/info" % port
            running: dict = {}
            busy, prompts = 0, set()
            deadline = time.time() + 5
            while time.time() < deadline:
                running = http_json(info_url, None, 2.0)
                busy = sum(1 for q in (running.get("queues") or []) if q.get("busy"))
                prompts = {x.get("prompt") for x in (running.get("recent") or [])}
                if busy >= 2 and {"TASK-A", "TASK-B"} <= prompts:
                    break
                time.sleep(0.05)
            assert busy >= 2, running
            assert {"TASK-A", "TASK-B"} <= prompts, running.get("recent")

            for th in threads:
                th.join()
            assert out["a"].get("status") == "success", out
            assert out["b"].get("status") == "success", out
            time.sleep(0.2)
            replies = [e for e in events if e.get("type") == "reply"]
            reply_text = "".join(t for t in ((e.get("data") or {}).get("text") for e in replies) if t)
            assert "done TASK-A" in reply_text and "done TASK-B" in reply_text, replies
            assert all((e.get("data") or {}).get("queue") == 0 for e in replies), replies
            if "resp" in sse_holder:
                sse_holder["resp"].close()
            th_sse.join(timeout=2)
            log("queues=2: 两个活动任务并发, 两条 reply 都到 master SSE, recent 汇总两条")

        # 每个队列一条在跑, 再排一条; reset 必须给所有提交方明确回 cancelled。
        out = {}
        keys = ["a", "b", "c"][:queues + 1]
        names = ["TASK-A", "TASK-B", "TASK-C"][:queues + 1]
        threads = [threading.Thread(target=submit, args=(url, name, out, key))
                   for key, name in zip(keys, names)]
        for th in threads:
            th.start()
            time.sleep(0.12)
        wait_pending(port, 1)
        reset = http_json("http://127.0.0.1:%d/reset" % port, {"scope": "task"}, 5.0)
        assert reset.get("cancelled") is True, reset
        for th in threads:
            th.join(timeout=8)
            assert not th.is_alive(), "reset 后提交方仍悬挂"
        for key in keys:
            assert out.get(key, {}).get("status") == "cancelled", out

        info = http_json("http://127.0.0.1:%d/info" % port, None, 2.0)
        assert info.get("pendingTasks") == 0, info
        assert not any(q.get("busy") for q in (info.get("queues") or [])), info
        time.sleep(1.8)  # 大于 mock 延迟: 排队任务不得在 reset 后又被悄悄派发
        for key in keys:
            assert out[key].get("status") == "cancelled", out
        log("queues=%d: 在跑 + 排队任务 reset 均回 cancelled, 无重入派发" % queues)
    finally:
        if "resp" in sse_holder:
            sse_holder["resp"].close()
        kill_all(procs)
        llm.shutdown()


def run_remote_case(agent_cli: str, agent_node: str, root: str) -> None:
    """远端命令已经启动后 reset: node 进程必须停, 不能只留下 cancelled 回包。"""
    ws_node = os.path.join(root, "node")
    ws_cli = os.path.join(root, "cli")
    logs = os.path.join(root, "logs")
    for d in (ws_node, ws_cli, logs):
        os.makedirs(d, exist_ok=True)
    started = os.path.join(ws_node, "started.txt")
    finished = os.path.join(ws_node, "finished.txt")
    with open(os.path.join(ws_node, "sleeper.py"), "w", encoding="utf-8") as f:
        f.write("import time\n"
                "open('started.txt', 'w').write('started')\n"
                "time.sleep(3)\n"
                "open('finished.txt', 'w').write('finished')\n")

    MockLlm.rounds = [
        ("tool", {"host": "nodeA", "command": "python sleeper.py", "timeout_ms": 60000}),
        ("text", "remote command unexpectedly completed"),
    ]
    MockLlm.hits = 0
    llm = ThreadingHTTPServer(("127.0.0.1", 0), QuietMockLlm)
    threading.Thread(target=llm.serve_forever, daemon=True).start()
    model_url = "http://127.0.0.1:%d/v1/chat/completions" % llm.server_address[1]
    port_node, port_cli = free_port(), free_port()
    token = "node-cancel"
    cfg_node = os.path.join(root, "node.json")
    cfg_cli = os.path.join(root, "cli.json")
    write_json(cfg_node, {
        "nodeName": "nodeA", "mode": "link", "host": "127.0.0.1", "port": port_node,
        "token": token, "rootDir": fwd(ws_node), "allowTerminal": True,
        "maxConcurrent": 1, "recycleSec": 0, "logFile": fwd(os.path.join(logs, "node.log")),
    })
    write_json(cfg_cli, {
        "agentName": "agent_cli", "logFile": fwd(os.path.join(logs, "cli.log")),
        "llm": {"url": model_url, "name": "mock", "maxRounds": 5, "maxRetries": 0},
        "task": {"queues": 1, "maxPending": 2},
        "terminal": {"rootDir": fwd(ws_cli), "allowTerminal": True, "allowFileTools": True},
        "network": {"enabled": False},
        "http": {"bind": "127.0.0.1", "port": port_cli, "token": ""},
        "nodes": [{"name": "nodeA", "host": "127.0.0.1", "port": port_node, "token": token}],
    })

    procs = []
    try:
        procs.append(start_proc(agent_node, cfg_node, os.path.join(logs, "node.out.txt"),
                                os.path.dirname(agent_node)))
        assert wait_tcp(port_node, 20)
        procs.append(start_proc(agent_cli, cfg_cli, os.path.join(logs, "cli.out.txt"),
                                os.path.dirname(agent_cli)))
        assert wait_tcp(port_cli, 20)
        deadline = time.time() + 10
        while time.time() < deadline:
            info = http_json("http://127.0.0.1:%d/info" % port_cli, None, 2.0)
            nodes = info.get("nodes") or []
            if nodes and nodes[0].get("online"):
                break
            time.sleep(0.1)
        assert nodes and nodes[0].get("online"), info

        out: dict = {}
        th = threading.Thread(target=submit, args=("http://127.0.0.1:%d/task" % port_cli,
                                                   "run remote sleeper", out, "remote"))
        th.start()
        deadline = time.time() + 8
        while time.time() < deadline and not os.path.isfile(started):
            time.sleep(0.05)
        assert os.path.isfile(started), "远端脚本未启动"
        http_json("http://127.0.0.1:%d/reset" % port_cli, {"scope": "task"}, 5.0)
        th.join(timeout=8)
        assert not th.is_alive() and out.get("remote", {}).get("status") == "cancelled", out
        time.sleep(3.5)
        assert not os.path.isfile(finished), "reset 后远端进程仍运行并写出了完成文件"
        log("remote_ssh: reset 真正停止 node 进程")
    finally:
        kill_all(procs)
        llm.shutdown()


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--agent-cli", required=True)
    ap.add_argument("--agent-node", default="")
    ap.add_argument("--tmp", default="")
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args(argv)
    agent_cli = os.path.abspath(args.agent_cli)
    tmp = os.path.abspath(args.tmp) if args.tmp else tempfile.mkdtemp(prefix="jz_task_cancel_")
    try:
        run_case(agent_cli, 1, os.path.join(tmp, "q1"))
        run_case(agent_cli, 2, os.path.join(tmp, "q2"))
        if args.agent_node:
            run_remote_case(agent_cli, os.path.abspath(args.agent_node), os.path.join(tmp, "remote"))
        log("PASS: standalone 队列取消/事件/历史/远端进程回归")
        return 0
    except Exception as e:
        log("FAIL: %r" % (e,))
        return 1
    finally:
        if not args.keep:
            import shutil
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
