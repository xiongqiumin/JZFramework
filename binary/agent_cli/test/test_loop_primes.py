#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""端到端测试: 无人值守驱动(HTTP 逐轮提交) + 验证程序(判定/备份/停止) + 外部复验。

被测的这条链路(每一环都有断言):
    agent_loop.py(驱动 + 守护)
      └─ 拉起 agent_cli(HTTP, 就是网页提交那套), 然后逐轮 POST /task {prompt, verify_cmd}
           ├─ 每轮: agent 改 solve.py -> 跑验证程序
           │      验证不通过 -> 输出原样喂回模型续跑(= 下一轮的输入, agent_cli 内建)
           │      验证通过   -> 验证程序自己: 备份这一版(仅复制, 不碰 git)
           │                    通过够 --min-rounds 轮 -> 在输出里打 [loop-stop]
           └─ 驱动在回包的 last_verify_output 里看见 [loop-stop] -> 不再提交下一轮
      └─ 跑 --recheck-cmd 外部复验(同一个验证程序 + --check-only) -> 通过 -> DONE, 退出码 0

用例(为什么选它): 质数筛 —— N 每轮随机取, 参考值由验证程序现场算, 判定是"两个整数是否相等"。
    比"算圆周率再比字符串前缀"那种判定硬得多: 没有浮点/精度/格式的模糊地带, 也没法把答案写死。

断言:
    1. 驱动退出码 0; supervisor.log 里有 READY / ROUND #1 / STOP / RECHECK OK / DONE
    2. 备份: <backups>/round_001/solve.py ... 每轮验证通过各留一份(至少 --min-rounds 份)
    3. 复验: 对每份备份单独跑 primes_verify.py --check-only --ws <备份目录> 都通过
    4. 每轮都拿到过回包(status/verify_rounds/用时都记进了 supervisor.log)
    5. 验证程序没碰版本控制(单独跑一次, 与上面的链路无关): 在一个**已有 .git 的空仓库**里跑它,
       跑完 git 仍然没有任何提交、也没有任何暂存 —— 只复制备份 + 记 rounds.log

⚠ 整条链路是真实模型在跑, 每轮几分钟到十几分钟不等(本地 4bit 模型思考慢), 所以 --timeout
默认给得很宽; 撞上它时先看日志(agent_cli_YYYYMMDD.log)是"模型还在干"还是"真卡住了"。

用法:
    python test_loop_primes.py --agent-cli <构建出来的 agent_cli.exe>
    (模型走 JZ_BENCH_MODEL_URL / JZ_BENCH_MODEL_NAME / JZ_BENCH_MODEL_TOKEN 或同名命令行参数)
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PRIMES_VERIFY = os.path.join(HERE, "primes_verify.py")            # 本用例的验证程序(同目录)
AGENT_LOOP = os.path.normpath(os.path.join(HERE, "..", "tools", "agent_loop.py"))   # 部署用的守护脚本

DEFAULT_MODEL_URL = "http://172.17.160.147:8080/v1/chat/completions"
DEFAULT_MODEL_NAME = "my_model"

PROMPT = """在工作区根目录写/改进 solve.py: 一个单线程的 Python 程序, 运行 python solve.py <N> 打印一行两个整数 —— N 以内(含 N)的质数个数 与 质数之和, 空格分隔(例: python solve.py 10 应该输出 4 17)。
要求:
1. 只许单线程: 不要 import threading / multiprocessing / concurrent / _thread;
2. 要快: N 在 100000 到 300000 之间时这条命令的用时必须 <= 1.0 秒; 每一轮都要比上一轮更快(上一轮用时见工作区里的 verify_result.json);
3. 每一轮只做一个明确的小改进, 改完自己跑一遍确认(例: python solve.py 100000)输出的结果正确。
工作区是空的(只有 solve.py 和上一轮留下的 verify_result.json), 不用探索目录; 自测一条命令就够。
"""

# "验证程序不碰版本控制"检查用的被测程序(埃氏筛, 必然通过)
GIT_SAFE_SOLVE = """
import sys


def solve(n):
    if n < 2:
        return 0, 0
    s = bytearray([1]) * (n + 1)
    s[0] = s[1] = 0
    i = 2
    while i * i <= n:
        if s[i]:
            s[i * i::i] = bytearray(len(range(i * i, n + 1, i)))
        i += 1
    return sum(s), sum(k for k in range(2, n + 1) if s[k])


n = int(sys.argv[1])
c, t = solve(n)
print(c, t)
"""


def log(msg: str = "") -> None:
    # Windows 控制台是本地编码(GBK): 日志里可能有它编不出的字符, 换成 '?' 而不是抛异常
    try:
        print(msg, flush=True)
    except UnicodeEncodeError:
        enc = (sys.stdout.encoding or "utf-8")
        print(msg.encode(enc, "replace").decode(enc, "replace"), flush=True)


def fwd(path: str) -> str:
    """Windows 路径换成 '/' —— 会经过 shell 规则拆词的地方(agent_loop 的 shlex.split)吃反斜杠。"""
    return os.path.abspath(path).replace("\\", "/")


def read_text(path: str) -> str:
    """日志按本地实际编码读: supervisor.log 是本脚本写的 UTF-8; agent_cli 的
    jzInfo 在 Windows 上会转成本地编码(GBK)再打 —— 两种都兜住, 断言只认 ASCII 标记。"""
    try:
        data = open(path, "rb").read()
    except OSError:
        return ""
    for enc in ("utf-8", "gbk", "latin-1"):
        try:
            return data.decode(enc)
        except Exception:                                    # noqa: BLE001
            continue
    return data.decode("utf-8", "replace")


def read_cli_log(log_dir: str) -> str:
    text = ""
    for p in sorted(glob.glob(os.path.join(log_dir, "agent_cli_*.log"))):
        text += read_text(p)
    return text


def find_agent_cli(explicit: str) -> str:
    if explicit:
        if not os.path.isfile(explicit):
            raise SystemExit("agent_cli 不存在: " + explicit)
        return fwd(explicit)
    # 从脚本位置往上找 build/, 再在 build 里按部署路径找(与 agent_cli_test 的探测同思路)
    d = HERE
    for _ in range(12):
        cand_dirs = [os.path.join(d, "build", "binary", "agent_cli", "bin", cfg)
                     for cfg in ("Release", "Debug")]
        cand_dirs += [os.path.join(d, "build", "binary", "agent_cli", cfg) for cfg in ("Release", "Debug")]
        for c in cand_dirs:
            for name in ("agent_cli.exe", "agent_cli"):
                p = os.path.join(c, name)
                if os.path.isfile(p):
                    return fwd(p)
        parent = os.path.dirname(d)
        if parent == d:
            break
        d = parent
    raise SystemExit("没找到 agent_cli(请 --agent-cli <路径>, 或先构建 agent_cli)")


def kill_tree(pid: int) -> None:
    """连子孙一起收: Windows 用 taskkill /T, POSIX 用信号发给进程组。"""
    if os.name == "nt":
        subprocess.run(["taskkill", "/T", "/F", "/PID", str(pid)],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return
    try:
        os.killpg(os.getpgid(pid), 15)
    except Exception:                                        # noqa: BLE001
        pass


def free_port() -> int:
    """要一个当前空闲的端口(agent_cli 的 HTTP 监听; 避免和别的服务/上一次跑冲突)。"""
    import socket
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def write_config(path: str, ws: str, python_exe: str, url: str, name: str, token: str,
                 http_port: int) -> None:
    cfg = {
        "agentName": "agent_cli_loop_test",
        "logFile": "",
        "llm": {
            "url": url,
            "name": name,
            "token": token,
            "netTimeoutMs": 120000,
            "maxRounds": 40,
            "maxToken": 160000,
            "maxTokens": 4096,
            "maxRetries": 3,
            "maxFixRetries": 2,
            "retryDelayMs": 1000,
        },
        "loop": {"enabled": True},
        # 每轮的"验证脚本门控"预算: 验证失败最多喂回 6 轮
        "verify": {"maxRounds": 6, "timeoutMs": 60000},
        "memory": {"enabled": False},
        "terminal": {
            "rootDir": fwd(ws),
            "programs": {"python": fwd(python_exe)},   # agent 要能自己跑 python solve.py <N>
            "allowTerminal": True,
            "allowFileTools": True,
            "allowPackageManager": False,
            "allowNetwork": False,
            "unrestricted": False,
        },
        "network": {"enabled": False},
        # 驱动就是往这个 HTTP 提交任务(agent_loop.py 会从这份 config.json 里读出 URL/token)
        "http": {"bind": "127.0.0.1", "port": http_port, "token": ""},
    }
    with open(path, "w", encoding="utf-8") as f:
        json.dump(cfg, f, ensure_ascii=False, indent=2)


def check_no_git_touch(tmp: str, python_exe: str, args, check, log) -> None:
    """验证程序**不许**碰版本控制: 在一个已有 .git 的空仓库里跑它一次, 跑完必须
    "0 次提交 + index 干净"(只复制备份 + 记 rounds.log)。

    为什么单开这一步: 上面那条链路的工作区要留给模型干干净净的(有 .git 它会跑去翻);
    而"会不会误提交"这件事本身可以用确定性方式验, 不必等模型跑完。
    """
    if not shutil.which("git"):
        log("  [skip] 没找到 git: 跳过\"验证程序不碰版本控制\"这条")
        return
    d = os.path.join(tmp, "git_safety")
    back = os.path.join(d, "backups")
    os.makedirs(back, exist_ok=True)
    with open(os.path.join(d, "solve.py"), "w", encoding="utf-8") as f:
        f.write(GIT_SAFE_SOLVE)
    subprocess.run(["git", "init", "-q", d], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    p = subprocess.run([python_exe, fwd(PRIMES_VERIFY), "--backup-dir", fwd(back),
                        "--min-n", "100000", "--max-n", "120000", "--target-sec", str(args.target_sec),
                        "--min-rounds", "99"],       # 99 = 这一轮绝不会打 [loop-stop]
                       cwd=d, capture_output=True, text=True, errors="replace", timeout=120)
    line = (p.stdout or "").strip().splitlines()
    check("版本控制安全检查: 验证程序能在已有仓库的工作区里跑通",
          p.returncode == 0 and "[verify-pass]" in (p.stdout or ""), line[0] if line else "")

    g = subprocess.run(["git", "-C", d, "log", "--oneline"], capture_output=True, text=True)
    commits = [ln for ln in g.stdout.splitlines() if ln.strip()]
    check("验证程序没有提交(仓库里 0 次提交)", len(commits) == 0, "实际 %d 次" % len(commits))

    st = subprocess.run(["git", "-C", d, "status", "--porcelain"], capture_output=True, text=True).stdout
    touched = [ln for ln in st.splitlines() if ln.strip() and not ln.startswith("??")]
    check("验证程序没有动 index(工作区里只有未跟踪文件)", not touched, "; ".join(touched)[:120])


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="agent_cli --loop 端到端测试(质数筛用例)")
    p.add_argument("--agent-cli", default=os.environ.get("JZ_AGENT_CLI", ""),
                   help="agent_cli 可执行文件(默认: 从脚本位置往上找 build)")
    p.add_argument("--agent-loop", default=AGENT_LOOP, help="驱动脚本(默认 tools/agent_loop.py)")
    p.add_argument("--python", default=sys.executable, help="跑验证程序/agent 用的 python")
    p.add_argument("--min-rounds", type=int, default=2, help="验证程序跑够几轮就 [loop-stop](默认 2)")
    p.add_argument("--target-sec", type=float, default=1.0, help="每轮用时上限(默认 1.0s)")
    p.add_argument("--max-rounds", type=int, default=6,
                   help="最多提交几轮(默认 6: 够 2 轮通过 + 几次重拉; 也保证模型不行时能收工)")
    p.add_argument("--timeout", type=float, default=2700.0,
                   help="整条链路的上限, 秒(默认 2700 = 45 分钟; 本地模型每轮波动很大)")
    p.add_argument("--tmp", default="", help="临时目录(默认自动建)")
    p.add_argument("--keep", action="store_true", help="跑完保留临时目录(排查用)")
    p.add_argument("--model-url", default=os.environ.get("JZ_BENCH_MODEL_URL", DEFAULT_MODEL_URL))
    p.add_argument("--model-name", default=os.environ.get("JZ_BENCH_MODEL_NAME", DEFAULT_MODEL_NAME))
    p.add_argument("--model-token", default=os.environ.get("JZ_BENCH_MODEL_TOKEN", ""))
    return p


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)

    agent_cli = find_agent_cli(args.agent_cli)
    python_exe = fwd(args.python)
    tmp = os.path.abspath(args.tmp) if args.tmp else tempfile.mkdtemp(prefix="jz_loop_primes_")
    ws = os.path.join(tmp, "ws")
    backups = os.path.join(tmp, "backups")
    logs = os.path.join(tmp, "logs")
    for d in (ws, backups, logs):
        os.makedirs(d, exist_ok=True)

    prompt_file = os.path.join(tmp, "prompt.txt")
    with open(prompt_file, "w", encoding="utf-8") as f:
        f.write(PROMPT)
    http_port = free_port()
    cfg_file = os.path.join(tmp, "config.json")
    write_config(cfg_file, ws, python_exe, args.model_url, args.model_name, args.model_token, http_port)

    log("=" * 78)
    log("agent_cli 无人值守端到端测试(HTTP 逐轮提交 + 质数筛验证程序)")
    log("  agent_cli : " + agent_cli)
    log("  python    : " + python_exe)
    log("  工作区    : " + fwd(ws))
    log("  备份目录  : " + fwd(backups))
    log("  日志目录  : " + fwd(logs))
    log("  HTTP 提交 : http://127.0.0.1:%d" % http_port)
    log("  模型      : %s @ %s" % (args.model_name, args.model_url))
    log("=" * 78)

    # 工作区保持"空目录 + 没有仓库": 模型一进来别去研究 .git("验证程序不碰版本控制"这条
    # 用下面的独立检查来验, 见 check_no_git_touch)

    verify = ('"%s" "%s" --backup-dir "%s" --min-n 100000 --max-n 300000 '
              '--target-sec %g --min-rounds %d'
              % (python_exe, fwd(PRIMES_VERIFY), fwd(backups), args.target_sec, args.min_rounds))
    recheck = verify + " --check-only"

    # 驱动: 拉起 agent_cli(-c 指定配置) -> 逐轮 POST /task -> 见到 [loop-stop] 收工 + 复验。
    # HTTP 地址/token 由 agent_loop.py 从这份 config.json 里读, 不用再抄一遍。
    agent_cmd = '"%s" -c "%s"' % (agent_cli, fwd(cfg_file))
    cmd = [python_exe, os.path.abspath(args.agent_loop),
           "--agent-cmd", agent_cmd,
           "--prompt-file", fwd(prompt_file),
           "--verify-cmd", verify,
           "--recheck-cmd", recheck,
           "--cwd", fwd(ws),
           "--log-dir", fwd(logs),
           "--retry-sec", "5",
           "--max-rounds", str(args.max_rounds)]
    log("[启动] " + " ".join('"%s"' % c if " " in c else c for c in cmd))
    log("-" * 78)

    proc = subprocess.Popen(cmd, cwd=tmp, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, errors="replace", bufsize=1)

    timed_out = {"v": False}

    def on_timeout():
        timed_out["v"] = True
        log("!! 超过 %.0f 秒还没结束, 收掉守护与它的子进程" % args.timeout)
        kill_tree(proc.pid)

    watchdog = threading.Timer(args.timeout, on_timeout)
    watchdog.start()
    try:
        assert proc.stdout is not None
        for line in proc.stdout:
            log("  | " + line.rstrip())
        rc = proc.wait()
    finally:
        watchdog.cancel()
        if proc.poll() is None:
            kill_tree(proc.pid)
            rc = proc.wait()

    log("-" * 78)
    sup_log = read_text(os.path.join(logs, "supervisor.log"))
    cli_log = read_cli_log(logs)
    rounds = sorted(glob.glob(os.path.join(backups, "round_*")))

    ok = True

    def check(name: str, cond: bool, extra: str = "") -> None:
        nonlocal ok
        log(("  [OK]   " if cond else "  [FAIL] ") + name + (("   " + extra) if extra else ""))
        if not cond:
            ok = False

    log("断言:")
    check("驱动退出码 0(见到停止标记 + 外部复验通过 -> 收工)", rc == 0,
          "rc=%s%s" % (rc, " (超时)" if timed_out["v"] else ""))
    check("supervisor.log 有 READY(agent_cli 的 HTTP 就绪)", "READY " in sup_log)
    check("supervisor.log 有 ROUND #1(真的逐轮提交过)", "ROUND #1" in sup_log)
    # ⚠ 别只找 "STOP": 达到 --max-rounds 也会打一行 STOP 开头的话 —— 这里要的是"看见了验证
    #   程序打的停止标记"那一种(见 agent_loop.py 的 stop_tag 分支)
    check("supervisor.log 有 STOP(看见了验证程序的停止标记)", "STOP 第" in sup_log)
    check("supervisor.log 有外部复验 RECHECK OK", "RECHECK OK" in sup_log)
    check("supervisor.log 有 DONE(目标达成, 没再提交)", "DONE" in sup_log)

    check("备份份数 >= %d" % args.min_rounds, len(rounds) >= args.min_rounds,
          "实际 %d: %s" % (len(rounds), ", ".join(os.path.basename(r) for r in rounds)))
    for r in rounds:
        check("备份 %s 里有 solve.py" % os.path.basename(r), os.path.isfile(os.path.join(r, "solve.py")))

    # 每份备份单独复验: "留下的那一版现在真的还通过"(同一个验证程序, 只判定不落盘)
    for r in rounds:
        p = subprocess.run([python_exe, fwd(PRIMES_VERIFY), "--check-only", "--ws", fwd(r),
                            "--min-n", "100000", "--max-n", "300000", "--target-sec", str(args.target_sec)],
                           cwd=r, capture_output=True, text=True, errors="replace", timeout=120)
        check("备份 %s 独立复验通过" % os.path.basename(r), p.returncode == 0,
              (p.stdout or "").strip().splitlines()[-1] if (p.stdout or "").strip() else "")

    if os.path.isfile(os.path.join(backups, "rounds.log")):
        log("  [info] rounds.log: " + " | ".join(read_text(os.path.join(backups, "rounds.log")).split()))
    # 守护的核心职责之一: 把子进程的 stdout/stderr 收进轮转日志。但 HTTP 模式下 agent_cli
    # 默认是安静的(就绪/每轮进展都不打 stdout, 验证输出走回包与 SSE) —— 所以这条只报不判。
    log("  [info] agent_cli 轮转日志: %d 字节(HTTP 模式下它默认不打 stdout, 空是正常的)" % len(cli_log))
    check_no_git_touch(tmp, python_exe, args, check, log)

    log("-" * 78)
    log("产物:")
    log("  工作区      : " + fwd(ws))
    log("  备份        : " + fwd(backups))
    log("  日志        : " + fwd(logs))
    log("  supervisor  : " + fwd(os.path.join(logs, "supervisor.log")))

    if not ok or args.keep or args.tmp:
        log("  (临时目录保留)")
    else:
        shutil.rmtree(tmp, ignore_errors=True)
        log("  (临时目录已删除; 要保留加 --keep)")

    log("=" * 78)
    log("结果: %s" % ("PASS" if ok and not timed_out["v"] else "FAIL"))
    return 0 if (ok and not timed_out["v"]) else 1


if __name__ == "__main__":
    sys.exit(main())
