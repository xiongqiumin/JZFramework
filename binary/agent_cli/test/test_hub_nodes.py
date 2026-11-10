#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""端到端测试: server -> cli -> node 全链路(managed 模式)。

链路(docs/agent-hub-design.md §2/§3):
    agent_cli_server(hub :8090, JZNetServer)
        ▲ JZNetClient 出站连接(token)          ← 控制面
      agent_cli(managed: **不起 HTTP、不起 frp**)
        └─ nodes[]: 两台 link 模式 agent_node(JZNetServer + token)   ← 执行面
             └─ 模型只看到一个 remote_ssh, 按 host 路由到 nodeA / nodeB

任务从 **server** 提交:`POST /api/tasks {agentId, prompt, verify_cmd, ...}`,
server 经控制面把任务下发给 cli, cli 用 remote_ssh 把 nodeA 的 secret 搬到 nodeB。

断言(硬):
  1. server 的 /api/status 里能看到这台 cli 上线(online[]), 且它上报的两台 node 都 online;
  2. server 提交任务返回 status == "success";
  3. nodeB 的 answer.txt 与 nodeA 的 secret.txt 逐字符相等;
  4. **cli 的 http.port 没有被监听** —— managed 与 standalone 互斥(不对外起 HTTP)。

用法:
    python test_hub_nodes.py --agent-cli <agent_cli.exe> --agent-node <agent_node.exe> \
                             --agent-server <agent_cli_server.exe>
"""
from __future__ import annotations

import argparse
import json
import os
import socket
import sys
import tempfile
import time
import urllib.request
import uuid

# 复用 remote_ssh 用例里的 mock 模型与工具函数(同一目录, 一起维护)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_remote_ssh_nodes import (  # noqa: E402
    PROMPT, free_port, fwd, http_json, kill_all, log,
    start_mock_llm, start_proc, wait_tcp, write_json,
)


def wait_server_online(http_port: int, agent_id: str, timeout: float) -> dict:
    """等 server 看到这台 cli 上线, 且它上报的 node 全部 online。"""
    deadline = time.time() + timeout
    last = {}
    while time.time() < deadline:
        try:
            st = http_json("http://127.0.0.1:%d/api/status" % http_port, None, 2.0)
            last = st
            for a in (st.get("online") or []):
                if a.get("agentId") != agent_id:
                    continue
                nodes = a.get("nodes") or []
                if nodes and all(n.get("online") for n in nodes):
                    return a
        except Exception:                                 # noqa: BLE001
            pass
        time.sleep(0.3)
    return last.get("online") or {}


def port_closed(port: int) -> bool:
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=0.5):
            return False
    except OSError:
        return True


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--agent-cli", default="")
    ap.add_argument("--agent-node", default="")
    ap.add_argument("--agent-server", default="")
    ap.add_argument("--python", default=sys.executable)
    ap.add_argument("--task-timeout", type=float, default=300.0)
    ap.add_argument("--tmp", default="")
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args(argv)

    agent_cli = os.path.abspath(args.agent_cli)
    agent_node = os.path.abspath(args.agent_node)
    agent_server = os.path.abspath(args.agent_server)
    for path, name in ((agent_cli, "agent_cli"), (agent_node, "agent_node"),
                       (agent_server, "agent_cli_server")):
        if not os.path.isfile(path):
            raise SystemExit("没有 %s: %s" % (name, path))

    tmp = os.path.abspath(args.tmp) if args.tmp else tempfile.mkdtemp(prefix="jz_hub_nodes_")
    ws_a = os.path.join(tmp, "nodeA")
    ws_b = os.path.join(tmp, "nodeB")
    ws_cli = os.path.join(tmp, "cli")
    logs = os.path.join(tmp, "logs")
    for d in (ws_a, ws_b, ws_cli, logs):
        os.makedirs(d, exist_ok=True)

    secret = "JZ-" + uuid.uuid4().hex[:16].upper()
    with open(os.path.join(ws_a, "secret.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write(secret + "\n")

    mock_server, model_url = start_mock_llm(secret)

    port_a, port_b = free_port(), free_port()
    port_hub, port_http = free_port(), free_port()
    port_cli_http = free_port()          # managed 下这个端口**不该**被监听
    token_a = "tok-" + uuid.uuid4().hex[:8]
    token_b = "tok-" + uuid.uuid4().hex[:8]
    hub_token = "hub-" + uuid.uuid4().hex[:8]
    agent_id = "a1"

    cfg_a = os.path.join(tmp, "nodeA.json")
    cfg_b = os.path.join(tmp, "nodeB.json")
    for path, name, port, token, ws in (
        (cfg_a, "nodeA", port_a, token_a, ws_a),
        (cfg_b, "nodeB", port_b, token_b, ws_b),
    ):
        write_json(path, {
            "nodeName": name, "mode": "link", "host": "127.0.0.1", "port": port,
            "token": token, "rootDir": fwd(ws), "allowTerminal": True, "maxConcurrent": 1,
            "recycleSec": 0, "logFile": fwd(os.path.join(logs, name + ".log")),
        })

    verify_py = os.path.join(tmp, "verify_copy.py")
    with open(verify_py, "w", encoding="utf-8") as f:
        f.write(
            "import sys\n"
            "a = open(sys.argv[1], 'rb').read().decode('utf-8', 'replace').strip()\n"
            "b = open(sys.argv[2], 'rb').read().decode('utf-8', 'replace').strip()\n"
            "if a != b:\n"
            "    print('MISMATCH: nodeA=%r nodeB=%r' % (a, b))\n"
            "    sys.exit(1)\n"
            "print('OK: ' + b)\n"
        )
    verify_cmd = '"%s" "%s" "%s" "%s"' % (fwd(args.python), fwd(verify_py),
                                          fwd(os.path.join(ws_a, "secret.txt")),
                                          fwd(os.path.join(ws_b, "answer.txt")))

    # cli: managed(server.host 非空) —— 刻意保留 http 段, 用来断言"它没有被监听"
    cfg_cli = os.path.join(tmp, "cli.json")
    write_json(cfg_cli, {
        "agentName": "agent_cli",
        "logFile": fwd(os.path.join(logs, "agent_cli.log")),
        "llm": {"url": model_url, "name": "mock", "token": "",
                "maxRounds": 40, "netTimeoutMs": 300000},
        "verify": {"maxRounds": 3, "timeoutMs": 120000},
        "terminal": {
            "rootDir": fwd(ws_cli), "allowTerminal": True, "allowFileTools": True,
            "allowPackageManager": False, "allowNetwork": False, "unrestricted": False,
        },
        "network": {"enabled": False},
        "server": {"host": "127.0.0.1", "port": port_hub, "token": hub_token, "agentId": agent_id},
        "http": {"bind": "127.0.0.1", "port": port_cli_http, "token": ""},
        "nodes": [
            {"name": "nodeA", "host": "127.0.0.1", "port": port_a, "token": token_a},
            {"name": "nodeB", "host": "127.0.0.1", "port": port_b, "token": token_b},
        ],
    })

    cfg_server = os.path.join(tmp, "server.json")
    write_json(cfg_server, {
        # 本机回归不备证书: server 默认强制 HTTPS+mTLS, 这里显式关掉(仅测试)
        "http": {"bind": "127.0.0.1", "port": port_http, "ssl": {"enabled": False}},
        "hub": {"bind": "127.0.0.1", "port": port_hub, "token": hub_token},
        "frp": {"server": {"enabled": False}},
        "agents": [],
    })

    log("=" * 78)
    log("server -> cli -> node 全链路(managed): 任务从 server 提交, 数据 nodeA -> nodeB")
    log("  agent_cli_server : http=:%d  hub=:%d" % (port_http, port_hub))
    log("  nodeA / nodeB    : :%d / :%d" % (port_a, port_b))
    log("  临时目录         : " + fwd(tmp))
    log("=" * 78)

    procs = []
    try:
        node_a = start_proc(agent_node, cfg_a, os.path.join(logs, "nodeA.out.txt"), os.path.dirname(agent_node))
        procs.append(node_a)
        node_b = start_proc(agent_node, cfg_b, os.path.join(logs, "nodeB.out.txt"), os.path.dirname(agent_node))
        procs.append(node_b)
        if not wait_tcp(port_a, 20) or not wait_tcp(port_b, 20):
            log("FAIL: 节点没起来(看 logs/node*.out.txt)")
            return 1

        server = start_proc(agent_server, cfg_server, os.path.join(logs, "server.out.txt"),
                            os.path.dirname(agent_server))
        procs.append(server)
        if not wait_tcp(port_http, 20) or not wait_tcp(port_hub, 20):
            log("FAIL: server 没起来(看 logs/server.out.txt)")
            return 1
        log("server 已起来: http=:%d hub=:%d" % (port_http, port_hub))

        cli = start_proc(agent_cli, cfg_cli, os.path.join(logs, "cli.out.txt"), os.path.dirname(agent_cli))
        procs.append(cli)

        online = wait_server_online(port_http, agent_id, 25)
        if not isinstance(online, dict) or online.get("agentId") != agent_id:
            log("FAIL: cli 没在 server 上线(token / hub 端口 / hello 失败): "
                + json.dumps(online, ensure_ascii=False))
            return 1
        nodes = online.get("nodes") or []
        if not nodes or not all(n.get("online") for n in nodes):
            log("FAIL: cli 上报的 node 没全部在线: " + json.dumps(nodes, ensure_ascii=False))
            return 1
        log("cli 已上线, 节点: " + ", ".join("%s(%s)" % (n.get("name"), n.get("shell", "")) for n in nodes))

        started = time.time()
        try:
            resp = http_json("http://127.0.0.1:%d/api/tasks" % port_http, {
                "agentId": agent_id,
                "prompt": PROMPT,
                "verify_cmd": verify_cmd,
                "verify_timeout": 120000,
                "max_verify_rounds": 3,
                "max_retries": 2,
                "use_memory": False,
            }, args.task_timeout)
        except Exception as e:                            # noqa: BLE001
            log("FAIL: /api/tasks 请求失败: %r" % (e,))
            return 1

        cost = time.time() - started
        log("任务回包(%.1fs): status=%s rounds=%s" %
            (cost, resp.get("status"), resp.get("verify_rounds")))
        log("  agent_reply: " + (resp.get("agent_reply") or "")[:300])
        log("  verify_out : " + (resp.get("last_verify_output") or "")[:300])
        if resp.get("error"):
            log("  error      : " + str(resp.get("error"))[:300])

        answer_path = os.path.join(ws_b, "answer.txt")
        if not os.path.isfile(answer_path):
            log("FAIL: nodeB 没有生成 answer.txt")
            return 1
        got = open(answer_path, "rb").read().decode("utf-8", "replace").strip()
        if got != secret:
            log("FAIL: 内容不一致 nodeB=%r 期望=%r" % (got, secret))
            return 1
        if resp.get("status") != "success":
            log("FAIL: 内容搬对了, 但任务判定不是 success(验证命令可能没跑通)")
            return 1

        # 事件上行: server 侧应该看到 cli 推上来的事件(它没有 SSE, 这是唯一的实时出口)
        st = http_json("http://127.0.0.1:%d/api/status" % port_http, None, 5.0)
        a = next((x for x in (st.get("online") or []) if x.get("agentId") == agent_id), None)
        if not a or not a.get("lastEventType"):
            log("FAIL: 控制面没有收到任务事件(taskEvent 上行失败)")
            return 1
        log("最后一个上行事件: " + str(a.get("lastEventType")))

        # 互斥: managed 模式不该对外起 HTTP
        if not port_closed(port_cli_http):
            log("FAIL: managed 模式下 cli 仍然监听了 http.port=%d(应互斥)" % port_cli_http)
            return 1
        log("cli 的 http.port 未监听(managed 互斥规则成立)")

        log("PASS: 经 server 下发, nodeA 的 secret 已原样出现在 nodeB(answer.txt)")
        return 0
    finally:
        kill_all(procs)
        if mock_server is not None:
            mock_server.shutdown()
        if args.keep:
            log("保留临时目录: " + fwd(tmp))
        else:
            import shutil
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
