#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""多任务队列(task.queues)端到端测试。

被测行为:
  · cli 配 `task.queues: 2` ⇒ 两个队列, **每个队列一个独立 JZAgent/一套工具**;
  · 队列内串行(一个队列同一时刻只跑一个任务), 队列之间并发;
  · 全忙时新任务排队(有 maxPending 上限), 有队列空出来就派发。

断言(硬):
  1. /info 的 queues 数组长度 == 2;
  2. 两条任务同时在跑时, 两条队列的 busy 都为 true(证明是两个 agent 并发, 不是假并发);
  3. 三条任务全部 status=success;
  4. 总耗时明显小于"串行三次"的时间 ⇒ 证明第一条与第二条真的并行。

用法:
    python test_task_queues.py --agent-cli <agent_cli.exe> [--delay 1.5]
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import tempfile
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_remote_ssh_nodes import (  # noqa: E402
    free_port, fwd, http_json, kill_all, log, start_proc, wait_tcp, write_json,
)

# 每个请求都会睡这么久: 并行/串行的差别由它放大, 断言才稳
DELAY = 1.5


class SlowLlm(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):        # 不刷屏
        pass

    def _body(self) -> bytes:
        if "chunked" in (self.headers.get("Transfer-Encoding") or "").lower():
            data = b""
            while True:
                line = self.rfile.readline().strip()
                if not line:
                    break
                n = int(line.split(b";")[0], 16)
                if n == 0:
                    self.rfile.readline()
                    break
                data += self.rfile.read(n)
                self.rfile.read(2)
            return data
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n) if n else b""

    def do_POST(self):                        # noqa: N802
        raw = self._body().decode("utf-8", "replace")
        marker = "?"
        for m in ("TASK-A", "TASK-B", "TASK-C"):
            if m in raw:
                marker = m
                break
        time.sleep(DELAY)                     # 关键: 让任务真的"占住"队列

        chunk = {
            "id": "mock", "object": "chat.completion.chunk", "created": 0, "model": "mock",
            "choices": [{"index": 0,
                         "delta": {"content": "done " + marker},
                         "finish_reason": "stop"}],
            "usage": {"prompt_tokens": 5, "completion_tokens": 5, "total_tokens": 10},
        }
        body = ("data: " + json.dumps(chunk, ensure_ascii=False) + "\n\n"
                + "data: [DONE]\n\n").encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
        self.wfile.flush()


def submit(url: str, prompt: str, out: dict, key: str) -> None:
    t0 = time.time()
    try:
        out[key] = http_json(url, {"prompt": prompt, "use_memory": False}, 120.0)
    except Exception as e:                    # noqa: BLE001
        out[key] = {"error": repr(e)}
    out[key + "_elapsed"] = time.time() - t0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--agent-cli", default="")
    ap.add_argument("--tmp", default="")
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--queues", type=int, default=2)
    args = ap.parse_args(argv)

    agent_cli = os.path.abspath(args.agent_cli)
    if not os.path.isfile(agent_cli):
        raise SystemExit("没有 agent_cli: %s" % agent_cli)

    tmp = os.path.abspath(args.tmp) if args.tmp else tempfile.mkdtemp(prefix="jz_queues_")
    ws = os.path.join(tmp, "cli")
    logs = os.path.join(tmp, "logs")
    for d in (ws, logs):
        os.makedirs(d, exist_ok=True)

    llm = ThreadingHTTPServer(("127.0.0.1", 0), SlowLlm)
    threading.Thread(target=llm.serve_forever, daemon=True).start()
    model_url = "http://127.0.0.1:%d/v1/chat/completions" % llm.server_address[1]

    port = free_port()
    cfg = os.path.join(tmp, "cli.json")
    write_json(cfg, {
        "agentName": "agent_cli",
        "logFile": fwd(os.path.join(logs, "agent_cli.log")),
        "llm": {"url": model_url, "name": "mock", "token": "",
                "maxRounds": 10, "netTimeoutMs": 60000},
        "verify": {"maxRounds": 1, "timeoutMs": 30000},
        "task": {"queues": args.queues, "maxPending": 8},
        "terminal": {"rootDir": fwd(ws), "allowTerminal": True, "allowFileTools": True},
        "network": {"enabled": False},
        "http": {"bind": "127.0.0.1", "port": port, "token": ""},
    })

    log("=" * 78)
    log("任务队列测试: queues=%d, 每个请求 %.1fs" % (args.queues, DELAY))
    log("  agent_cli : " + fwd(agent_cli))
    log("  http      : http://127.0.0.1:%d" % port)
    log("  临时目录  : " + fwd(tmp))
    log("=" * 78)

    procs = []
    try:
        procs.append(start_proc(agent_cli, cfg, os.path.join(logs, "cli.out.txt"),
                                os.path.dirname(agent_cli)))
        if not wait_tcp(port, 30):
            log("FAIL: agent_cli 没起来(看 logs/cli.out.txt)")
            return 1

        info = http_json("http://127.0.0.1:%d/info" % port, None, 5.0)
        queues = info.get("queues") or []
        if len(queues) != args.queues:
            log("FAIL: /info 的 queues 数量 %d != %d" % (len(queues), args.queues))
            return 1
        log("队列数正确: %d" % len(queues))

        url = "http://127.0.0.1:%d/task" % port
        out: dict = {}
        t0 = time.time()

        # A、B 同时提交: 应该各占一个队列
        th_a = threading.Thread(target=submit, args=(url, "TASK-A 请直接答复", out, "a"))
        th_b = threading.Thread(target=submit, args=(url, "TASK-B 请直接答复", out, "b"))
        th_a.start()
        th_b.start()

        # 等它们都真的开跑, 再看 /info: 两条队列都应 busy
        busy = 0
        last = {}
        for _ in range(30):
            time.sleep(0.1)
            last = http_json("http://127.0.0.1:%d/info" % port, None, 5.0)
            busy = sum(1 for q in (last.get("queues") or []) if q.get("busy"))
            if busy >= 2:
                break
        if busy < 2:
            th_a.join()
            th_b.join()
            log("FAIL: 只看到 %d 条队列在跑(应为 2) —— 队列没有真正并发" % busy)
            log("  /info queues: " + json.dumps(last.get("queues"), ensure_ascii=False))
            log("  /info pending: %s busy=%s" % (last.get("pendingTasks"), last.get("busy")))
            for k in ("a", "b"):
                log("  %s: %s" % (k, json.dumps(out.get(k), ensure_ascii=False)[:300]))
            return 1
        log("两条队列同时在跑(busy=%d) —— 并发成立" % busy)

        # 队列都满时再提交 C: 应进排队, 有队列空出来后自动跑
        th_c = threading.Thread(target=submit, args=(url, "TASK-C 请直接答复", out, "c"))
        th_c.start()
        time.sleep(0.4)
        st = http_json("http://127.0.0.1:%d/info" % port, None, 5.0)
        if not st.get("pendingTasks"):
            log("FAIL: 全忙时提交的任务没有进排队(pendingTasks=0)")
            return 1
        log("第三条任务已排队(pending=%d)" % st["pendingTasks"])

        th_a.join()
        th_b.join()
        th_c.join()
        cost = time.time() - t0

        for k in ("a", "b", "c"):
            r = out.get(k) or {}
            log("  %s: status=%s  %.2fs" % (k, r.get("status"), out.get(k + "_elapsed", 0)))
            if r.get("status") != "success":
                log("FAIL: 任务 %s 没有成功: %s" % (k, json.dumps(r, ensure_ascii=False)[:300]))
                return 1

        # 串行跑三条 = 3*DELAY; 两条并发 + 一条排队 ≈ 2*DELAY
        limit = DELAY * 2 + 1.2
        if cost > limit:
            log("FAIL: 总耗时 %.1fs > %.1fs —— 看起来是串行执行的" % (cost, limit))
            return 1
        log("总耗时 %.1fs(< %.1fs, 串行应为 %.1fs) —— 队列并发生效" %
            (cost, limit, DELAY * 3))

        st = http_json("http://127.0.0.1:%d/info" % port, None, 5.0)
        if st.get("pendingTasks"):
            log("FAIL: 结束后仍有排队任务")
            return 1

        log("PASS: 多队列并发 + 排队派发都正常")
        return 0
    finally:
        kill_all(procs)
        llm.shutdown()
        if args.keep:
            log("保留临时目录: " + fwd(tmp))
        else:
            import shutil
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
