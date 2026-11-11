#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""端到端测试: 2 个 link 模式 agent_node + 1 个 agent_cli, 把 nodeA 的数据拷到 nodeB。

被测链路(docs/agent-hub-design.md §6):
    agent_cli(standalone HTTP)
      └─ nodes[]: 两台 link 模式 agent_node(JZNetServer + token)
           └─ 模型只看到**一个** remote_ssh 工具, 按 host 路由到 nodeA / nodeB

两种跑法:
  1. 默认 **mock 模型**(不联网、秒级、可进快速回归):
     起一个假的 OpenAI 兼容服务, 按顺序回 5 轮 ——
       ① tool_call remote_ssh(host=nodeA, command="cat secret.txt")
       ② tool_call remote_ssh(host=nodeB, command="printf '%s' '<内容>' > answer.txt")
       ③ tool_call remote_ssh(host=nodeB, command="printf 'one\\n' > bg.txt; (sleep 1; 追写第二行) &")
       ④ tool_call remote_ssh(host=nodeB, command="sleep 2; cat bg.txt")
       ⑤ 最终回复 "done"
     ③~④ 覆盖"后台就是命令自己的 `&`"; 验证脚本断言 bg.txt 两行都在。
     它验证的是**本工程的链路**(协议 / 工具 / 验证循环), 不是模型能力。
  2. --real-llm: 用 <agent_cli 同目录>/test_llm.json 的真模型跑同一个任务(慢, 挂 llm 标签)。

断言(全部是"硬"的):
  1. 两台 node 都能被 cli 连上(/info 里 online=true), 否则 remote_ssh 会直接报离线;
  2. 任务回包 status == "success";
  3. nodeB 的 answer.txt 与 nodeA 的 secret.txt **逐字符相等**(去首尾空白);
  4. 验证命令只读 nodeB 的文件, 且 cli 本地工作区没有它 ⇒ 必须真的经 remote_ssh 搬运。

用法:
    python test_remote_ssh_nodes.py --agent-cli <agent_cli.exe> --agent-node <agent_node.exe>
    python test_remote_ssh_nodes.py ... --real-llm
"""
from __future__ import annotations

import argparse
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

LLM_JSON = "test_llm.json"

# ⚠ 刻意只给**一句话**: 节点清单与 remote_ssh 的用法都在工具描述里(见 JZRemoteSshTool),
#   所以这里不重复教它怎么用 —— 真模型能不能只凭这句话完成, 正是这个用例要测的。
PROMPT = "把 nodeA 工作区里的 secret.txt 内容原样写到 nodeB 工作区的 answer.txt 里。"


def log(msg: str = "") -> None:
    try:
        print(msg, flush=True)
    except UnicodeEncodeError:
        enc = (sys.stdout.encoding or "utf-8")
        print(msg.encode(enc, "replace").decode(enc, "replace"), flush=True)


def fwd(path: str) -> str:
    return os.path.abspath(path).replace("\\", "/")


def free_port() -> int:
    # Windows 上会有"刚关掉的端口又绑不上"(WinError 10013, 被 Hyper-V/端口排除区间占住),
    # 所以这里绑一次、关掉、再绑一次确认可用, 失败就换一个。
    for _ in range(30):
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
        s.close()
        try:
            t = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            t.bind(("127.0.0.1", port))
            t.close()
            return port
        except OSError:
            continue
    raise RuntimeError("找不到可用端口")


def wait_tcp(port: int, timeout: float) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.5):
                return True
        except OSError:
            time.sleep(0.2)
    return False


def http_json(url: str, body: dict | None, timeout: float) -> dict:
    data = None if body is None else json.dumps(body).encode("utf-8")
    req = urllib.request.Request(url, data=data,
                                 headers={"Content-Type": "application/json"},
                                 method="GET" if body is None else "POST")
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


def read_llm_config(exe_dir: str) -> dict:
    path = os.path.join(exe_dir, LLM_JSON)
    try:
        with open(path, "rb") as f:
            root = json.loads(f.read().decode("utf-8"))
    except Exception:                                     # noqa: BLE001
        return {"url": "", "name": "", "token": "", "path": path}
    llm = root.get("llm") if isinstance(root.get("llm"), dict) else {}
    return {
        "url": llm.get("url") or root.get("modelUrl", ""),
        "name": llm.get("name") or root.get("modelName", ""),
        "token": llm.get("token") or root.get("modelToken", ""),
        "path": path,
    }


def write_json(path: str, value: dict) -> None:
    with open(path, "w", encoding="utf-8") as f:
        json.dump(value, f, indent=2, ensure_ascii=False)


# ---------------------------------------------------------------------------
# mock 模型: 只回 3 轮(两次 remote_ssh + 一次收尾), 与真实模型"决定用工具"的
# 行为形状一致, 但不含任何随机性 —— 这样这条链路能进快速回归。
# ---------------------------------------------------------------------------
class MockLlm(BaseHTTPRequestHandler):
    # 由 main 注入: [(kind, payload), ...]; kind = "tool" / "text"
    rounds: list = []
    # 每轮对"上一条 tool 结果"的期望(任一命中即可; 空 = 不检查)。
    # 这是硬断言: 期望落空就记进 failures, 用例最后一定会失败 —— 不用去读节点日志
    # (日志有缓冲, 进程被杀时尾行可能根本落不了盘)。
    expects: list = []
    failures: list = []
    hits = 0
    lock = threading.Lock()
    protocol_version = "HTTP/1.1"     # 与真实服务一致; 响应带 Content-Length 才能复用连接

    def log_message(self, fmt, *args):                    # 不刷屏
        pass

    def _read_body(self) -> bytes:
        """读请求体。⚠ 客户端(JZNet/mongoose)对较大的 POST 用 **chunked** 发送,
        没有 Content-Length —— 只按 Content-Length 读会读到 0 字节, 于是回包先于请求体,
        mongoose 那边就只看到空 body(Syntax error)。"""
        if "chunked" in (self.headers.get("Transfer-Encoding") or "").lower():
            data = b""
            while True:
                line = self.rfile.readline().strip()
                if not line:
                    break
                size = int(line.split(b";")[0], 16)
                if size == 0:
                    self.rfile.readline()      # 结尾的空行
                    break
                data += self.rfile.read(size)
                self.rfile.read(2)             # 每块后面的 CRLF
            return data
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n) if n else b""

    def do_GET(self):                                     # noqa: N802
        # 自检用: 客户端先探 /v1/models 的话也能看到请求确实到了这里
        self.send_response(404)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_POST(self):                                    # noqa: N802
        raw = self._read_body()
        if os.environ.get("JZ_MOCK_DEBUG"):
            log("  mock llm: POST %s (%d 字节, hdrs=%s)" % (self.path, len(raw),
                                                          dict(self.headers.items())))
            try:
                req = json.loads(raw.decode("utf-8"))
                tools = [m for m in (req.get("messages") or []) if m.get("role") == "tool"]
                if tools:
                    log("  mock llm: 上一条 tool 结果 = " + (tools[-1].get("content") or "")[:300])
            except Exception:                             # noqa: BLE001
                pass

        # 取会话里最后一条 tool 结果(用于下面"上一轮是否真的做成了"的断言)
        last_tool = ""
        try:
            req = json.loads(raw.decode("utf-8"))
            tools = [m for m in (req.get("messages") or []) if m.get("role") == "tool"]
            if tools:
                last_tool = tools[-1].get("content") or ""
        except Exception:                                 # noqa: BLE001
            pass

        with MockLlm.lock:
            idx = min(MockLlm.hits, len(MockLlm.rounds) - 1)
            MockLlm.hits += 1
            kind, payload = MockLlm.rounds[idx]
            if idx < len(MockLlm.expects) and MockLlm.expects[idx]:
                if not any(e in last_tool for e in MockLlm.expects[idx]):
                    MockLlm.failures.append(
                        "round %d 期望 tool 结果含 %r, 实际 = %r"
                        % (idx, MockLlm.expects[idx], last_tool[:300]))

        # ⚠ agent_cli 开的是 **流式**(JZAgent::setStreamEnabled(true)): 响应必须是 SSE,
        #   否则 body 在 sigReadyRead 里就被当 SSE 消费掉, 到 onLLMRequest 只剩空包。
        if kind == "tool":
            delta = {
                "role": "assistant",
                "tool_calls": [{
                    "index": 0,
                    "id": "call_%d" % (idx + 1),
                    "type": "function",
                    "function": {"name": "remote_ssh",
                                 "arguments": json.dumps(payload, ensure_ascii=False)},
                }],
            }
            finish = "tool_calls"
        else:
            delta = {"content": payload}
            finish = "stop"

        chunk = {
            "id": "mock-%d" % idx,
            "object": "chat.completion.chunk",
            "created": 0,
            "model": "mock",
            "choices": [{"index": 0, "delta": delta, "finish_reason": finish}],
            "usage": {"prompt_tokens": 10, "completion_tokens": 10, "total_tokens": 20},
        }
        body = ("data: " + json.dumps(chunk, ensure_ascii=False) + "\n\n"
                + "data: [DONE]\n\n").encode("utf-8")

        try:
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            self.wfile.flush()
            if os.environ.get("JZ_MOCK_DEBUG"):
                log("  mock llm: 已回 %d 字节 (round=%d kind=%s)" % (len(body), idx, kind))
        except Exception as e:                            # noqa: BLE001
            log("  mock llm: 回包失败 %r" % (e,))


def start_mock_llm(secret: str) -> tuple[ThreadingHTTPServer, str]:
    # nodeB 上用 printf 精确写入(不带换行), 内容就是 nodeA 那份 secret
    # 第 3~4 轮覆盖"后台就是命令自己的 `&`"(remote_ssh 没有 background/query/cancel 了):
    #   先写一行, 再用子 shell 后台追写第二行, 然后 cat 文件确认两行都在。
    #   ⚠ `&` 立刻返回, 所以第 4 轮要 sleep 一下再 cat。
    MockLlm.rounds = [
        ("tool", {"host": "nodeA", "command": "cat secret.txt"}),
        ("tool", {"host": "nodeB", "command": "printf '%%s' '%s' > answer.txt" % secret}),
        ("tool", {"host": "nodeB",
                  "command": "printf 'one\\n' > bg.txt; (sleep 1; printf 'two\\n' >> bg.txt) &"}),
        ("tool", {"host": "nodeB", "command": "sleep 2; cat bg.txt"}),
        ("text", "done: secret 已搬运; 后台任务写入两行"),
    ]
    # 每轮发出去之前, 检查"上一轮的工具调用真的做成了"(见 MockLlm.expects)
    MockLlm.expects = [
        None, None, None,
        ["后台运行"],      # 上一轮 `&`: 启动即返回
        ["two"],          # 上一轮 cat bg.txt: 必须看到后台写的第二行
    ]
    MockLlm.failures = []
    MockLlm.hits = 0
    # 直接让系统分配端口(port=0)再读回来 —— 避免"先探测再绑定"那一下的竞态/端口排除
    server = ThreadingHTTPServer(("127.0.0.1", 0), MockLlm)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server, "http://127.0.0.1:%d/v1/chat/completions" % server.server_address[1]


def start_proc(exe: str, cfg: str, log_path: str, cwd: str):
    out = open(log_path, "wb")
    return subprocess.Popen([exe, "-c", cfg], cwd=cwd, stdout=out, stderr=subprocess.STDOUT)


def wait_http_info(port: int, timeout: float) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            http_json("http://127.0.0.1:%d/info" % port, None, 2.0)
            return True
        except Exception:                                 # noqa: BLE001
            time.sleep(0.4)
    return False


def wait_nodes_online(cli_port: int, timeout: float) -> dict:
    deadline = time.time() + timeout
    info = {}
    while time.time() < deadline:
        try:
            info = http_json("http://127.0.0.1:%d/info" % cli_port, None, 2.0)
            nodes = info.get("nodes") or []
            if nodes and all(n.get("online") for n in nodes):
                return info
        except Exception:                                 # noqa: BLE001
            pass
        time.sleep(0.3)
    return info


def kill_all(procs) -> None:
    for p in procs:
        if p is None or p.poll() is not None:
            continue
        try:
            p.terminate()
            p.wait(timeout=5)
        except Exception:                                 # noqa: BLE001
            try:
                p.kill()
            except Exception:                             # noqa: BLE001
                pass


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--agent-cli", default="")
    ap.add_argument("--agent-node", default="")
    ap.add_argument("--python", default=sys.executable)
    ap.add_argument("--task-timeout", type=float, default=300.0)
    ap.add_argument("--tmp", default="")
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--real-llm", action="store_true",
                    help="用 <agent_cli 同目录>/test_llm.json 的真模型(默认用内置 mock)")
    ap.add_argument("--model-url", default="")
    ap.add_argument("--model-name", default="")
    ap.add_argument("--model-token", default="")
    args = ap.parse_args(argv)

    agent_cli = os.path.abspath(args.agent_cli)
    agent_node = os.path.abspath(args.agent_node)
    if not os.path.isfile(agent_cli):
        raise SystemExit("没有 agent_cli: %s" % agent_cli)
    if not os.path.isfile(agent_node):
        raise SystemExit("没有 agent_node: %s" % agent_node)

    llm = read_llm_config(os.path.dirname(agent_cli))
    mock_server = None
    if args.real_llm:
        model_url = args.model_url or llm["url"]
        model_name = args.model_name or llm["name"]
        model_token = args.model_token or llm["token"]
        if not model_url or not model_name:
            log("SKIP: --real-llm 但没配模型(%s)" % fwd(llm["path"]))
            return 77
    else:
        model_url, model_name, model_token = "", "mock", ""

    tmp = os.path.abspath(args.tmp) if args.tmp else tempfile.mkdtemp(prefix="jz_remote_ssh_")
    ws_a = os.path.join(tmp, "nodeA")
    ws_b = os.path.join(tmp, "nodeB")
    ws_cli = os.path.join(tmp, "cli")
    logs = os.path.join(tmp, "logs")
    for d in (ws_a, ws_b, ws_cli, logs):
        os.makedirs(d, exist_ok=True)

    secret = "JZ-" + uuid.uuid4().hex[:16].upper()
    with open(os.path.join(ws_a, "secret.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write(secret + "\n")

    if not args.real_llm:
        mock_server, model_url = start_mock_llm(secret)

    port_a, port_b, port_cli = free_port(), free_port(), free_port()
    token_a, token_b = "tok-" + uuid.uuid4().hex[:8], "tok-" + uuid.uuid4().hex[:8]

    cfg_a = os.path.join(tmp, "nodeA.json")
    cfg_b = os.path.join(tmp, "nodeB.json")
    write_json(cfg_a, {
        "nodeName": "nodeA", "mode": "link", "host": "127.0.0.1", "port": port_a,
        "token": token_a, "rootDir": fwd(ws_a), "allowTerminal": True, "maxConcurrent": 1,
        "recycleSec": 0, "logFile": fwd(os.path.join(logs, "nodeA.log")),
    })
    write_json(cfg_b, {
        "nodeName": "nodeB", "mode": "link", "host": "127.0.0.1", "port": port_b,
        "token": token_b, "rootDir": fwd(ws_b), "allowTerminal": True, "maxConcurrent": 1,
        "recycleSec": 0, "logFile": fwd(os.path.join(logs, "nodeB.log")),
    })

    # 验证命令: 只读 nodeB 的 answer.txt, 参考值直接读 nodeA 的原始文件(本机路径, 只做判定)
    verify_py = os.path.join(tmp, "verify_copy.py")
    with open(verify_py, "w", encoding="utf-8") as f:
        f.write(
            "import os, sys\n"
            "a = open(sys.argv[1], 'rb').read().decode('utf-8', 'replace').strip()\n"
            "b = open(sys.argv[2], 'rb').read().decode('utf-8', 'replace').strip()\n"
            "if a != b:\n"
            "    print('MISMATCH: nodeA=%r nodeB=%r' % (a, b))\n"
            "    sys.exit(1)\n"
            # 后台任务: `&` 起的那条要真的跑完(两行都在)
            "p = sys.argv[3]\n"
            "if not os.path.isfile(p):\n"
            "    print('MISSING: 后台命令没有产出 %s' % p)\n"
            "    sys.exit(1)\n"
            "c = open(p, 'rb').read().decode('utf-8', 'replace')\n"
            "if c != 'one\\ntwo\\n':\n"
            "    print('BAD-BG: 期望后台写入两行, 实际 %r' % c)\n"
            "    sys.exit(1)\n"
            "print('OK: ' + b + ' / bg stopped')\n"
        )
    verify_cmd = '"%s" "%s" "%s" "%s" "%s"' % (fwd(args.python), fwd(verify_py),
                                              fwd(os.path.join(ws_a, "secret.txt")),
                                              fwd(os.path.join(ws_b, "answer.txt")),
                                              fwd(os.path.join(ws_b, "bg.txt")))

    cfg_cli = os.path.join(tmp, "cli.json")
    write_json(cfg_cli, {
        "agentName": "agent_cli",
        "logFile": fwd(os.path.join(logs, "agent_cli.log")),
        "llm": {
            "url": model_url, "name": model_name, "token": model_token,
            "maxRounds": 40, "netTimeoutMs": 300000,
        },
        "verify": {"maxRounds": 3, "timeoutMs": 120000},
        "terminal": {
            "rootDir": fwd(ws_cli),
            "allowTerminal": True, "allowFileTools": True,
            "allowPackageManager": False, "allowNetwork": False, "unrestricted": False,
        },
        "network": {"enabled": False},
        "http": {"bind": "127.0.0.1", "port": port_cli, "token": ""},
        "nodes": [
            {"name": "nodeA", "host": "127.0.0.1", "port": port_a, "token": token_a},
            {"name": "nodeB", "host": "127.0.0.1", "port": port_b, "token": token_b},
        ],
    })

    log("=" * 78)
    log("node A/B + cli 端到端(remote_ssh 跨节点搬数据)  模型=%s" %
        ("mock" if not args.real_llm else model_name))
    log("  agent_cli  : " + fwd(agent_cli))
    log("  agent_node : " + fwd(agent_node))
    log("  nodeA      : 127.0.0.1:%d  ws=%s" % (port_a, fwd(ws_a)))
    log("  nodeB      : 127.0.0.1:%d  ws=%s" % (port_b, fwd(ws_b)))
    log("  cli        : http://127.0.0.1:%d  ws=%s" % (port_cli, fwd(ws_cli)))
    log("  临时目录   : " + fwd(tmp))
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
        log("两台 node 已监听")

        cli = start_proc(agent_cli, cfg_cli, os.path.join(logs, "cli.out.txt"), os.path.dirname(agent_cli))
        procs.append(cli)
        if not wait_http_info(port_cli, 30):
            log("FAIL: agent_cli 的 HTTP 没起来(看 logs/cli.out.txt)")
            return 1

        info = wait_nodes_online(port_cli, 15)
        nodes = info.get("nodes") or []
        if not nodes or not all(n.get("online") for n in nodes):
            log("FAIL: 节点没上线(hello/helloAck 失败, 多半是 token/端口): "
                + json.dumps(nodes, ensure_ascii=False))
            return 1
        log("两台 node 已上线: " + ", ".join("%s(%s)" % (n["name"], n.get("shell", "")) for n in nodes))

        started = time.time()
        try:
            resp = http_json("http://127.0.0.1:%d/task" % port_cli, {
                "prompt": PROMPT,
                "verify_cmd": verify_cmd,
                "verify_timeout": 120000,
                "max_verify_rounds": 3,
                "max_retries": 2,
                "use_memory": False,
            }, args.task_timeout)
        except Exception as e:                            # noqa: BLE001
            log("FAIL: /task 请求失败: %r" % (e,))
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

        # ---- background / query / cancel: 两条硬证据 ----
        #  ① 每轮的 tool 结果都对(mock 侧断言, 见 MockLlm.expects);
        #  ② 等过后台命令自己的 sleep 再读文件: 第二行不能出现, 否则说明 cancel 没杀掉进程。
        if MockLlm.failures:
            log("FAIL: 后台/查询/取消的工具结果不对:")
            for f in MockLlm.failures:
                log("  - " + f)
            return 1

        time.sleep(2.0)     # 后台那条 (sleep 1) 来得及写完
        bg_path = os.path.join(ws_b, "bg.txt")
        bg = open(bg_path, "rb").read().decode("utf-8", "replace") if os.path.isfile(bg_path) else ""
        if bg != "one\ntwo\n":
            log("FAIL: 后台命令没写完两行(期望 'one\\ntwo\\n', 实际 %r)" % bg)
            return 1

        log("PASS: nodeA 的 secret 已原样出现在 nodeB(answer.txt); `&` 后台写入两行已确认")
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
