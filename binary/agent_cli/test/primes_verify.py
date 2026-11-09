#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""agent_cli 的验证程序 = "这一轮之后做什么"的策略所有者。

被谁、怎么调:
    提交任务时带 verify_cmd(HTTP POST /task 的请求体, 驱动见 tools/agent_loop.py):
        {"prompt": "...", "verify_cmd": "<python> primes_verify.py --backup-dir <目录>"}
    agent_cli 每轮(以及每次续跑)都会在**工作区根目录**(配置 terminal.rootDir)下跑它一次:
        退出码 0 = 通过(本轮结束); 非 0 = 不通过。
    不通过时它的输出会被 agent_cli **原样喂回模型**当输入继续改 —— 所以这里的失败信息
    要写清"错在哪 + 下一步怎么办", 它就是下一轮的输入("验证输出作为下一轮输入"这条链路由
    agent_cli 提供, 这里只负责把话说清楚)。

被测程序(约定): 工作区根目录的 solve.py —— `python solve.py <N>` 打印一行两个整数:
    N 以内(含 N)的质数个数 与 质数之和(空格分隔)。例: `python solve.py 10` -> "4 17"

为什么这么判(判定必须"一眼对错"):
    N 每轮**随机**取(见 --min-n/--max-n), 参考值由本脚本自己现场筛一遍算出来 —— 于是
    ① 判定是纯整数相等(没有浮点/字符串前缀那套模糊比较); ② 想"把答案写死"也没用(每轮 N 都变)。

它做三件事(判定 → 复制 → 停止):
    1. 判定: solve.py 必须"单线程 + 结果正确 + 用时 <= --target-sec";
    2. 复制: 通过就把这版程序整份拷到 <backup-dir>/round_<n>/ (n = 已有备份数 + 1) ——
       "每轮验证通过后都有备份", 后面某一轮改坏了也不至于丢掉已经通过验证的版本;
    3. 停止: 通过的轮数 >= --min-rounds 时, 在输出里打 [loop-stop] —— **提交方**(驱动)在
       本轮回包的 last_verify_output 里看见它, 就不再提交下一轮(收工前的复验见 --check-only)。
       CLI 不认识这个标记: "跑几轮 / 什么时候停"是提交方的决策, 见 tools/agent_loop.py。

    ⛔ 故意**不碰版本控制**: 不留 commit, 也不写 .git/index —— 工作区可能是别人的仓库,
    "验证通过就提交"太容易误提交(agent 自己提交是它的事, 验证程序不该替它做决定)。
    每轮只往 <backup-dir>/rounds.log 记一行(轮次/N/用时), 想回看时序看它。

--check-only: 只做第 1 步, 不复制 / 不发 [loop-stop]。
    用途是"外部复验": 循环结束后由守护(agent_loop.py --recheck-cmd)或人再跑一遍,
    确认**留下的那一版**现在真的还通过 —— 判定逻辑还是这一份, 不另起一套。
"""
from __future__ import annotations

import argparse
import os
import random
import re
import shutil
import subprocess
import sys
import time

STOP_TAG = "[loop-stop]"       # 打给 agent_cli 的控制信号(见 JZAgentCli 的 verifyRequestsStop)
FAIL_TAG = "[verify-fail]"     # 纯 ASCII 标记: 日志/测试按它判定, 不受控制台编码影响
PASS_TAG = "[verify-pass]"
NEXT_TAG = "[verify-next]"

# "单线程"的硬边界: 不许 import 并发库(线程/进程/协程池都算)
FORBIDDEN = re.compile(r"^\s*(?:import|from)\s+(threading|multiprocessing|concurrent|_thread)\b", re.M)

# 备份要带走的"程序文件"后缀(多文件程序也一起备份)
PROGRAM_SUFFIX = (".py", ".txt", ".json", ".md", ".sh", ".bat", ".cmd", ".c", ".cpp", ".h", ".ts", ".js")
SKIP_DIRS = {"__pycache__", "node_modules", "venv", ".git"}

RESULT_FILE = "verify_result.json"   # 本轮结果落盘, 给下一轮的模型读"上一轮多快"


def out(msg: str = "") -> None:
    """一行输出 + 立即 flush: 被 agent_cli 按块收走时不要卡在缓冲里。"""
    print(msg)
    sys.stdout.flush()


def force_utf8_stdout() -> None:
    """Windows 上 Python 默认按控制台编码(cp936)写管道, 而 agent_cli 按 UTF-8 收 ——
    不掰成 UTF-8 的话, 中文失败原因到模型那边就是乱码。"""
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:                                        # noqa: BLE001
        pass


def reference_primes(n: int):
    """参考实现(埃氏筛): 返回 (n 以内质数个数, 质数之和)。

    只用来判定被测程序算得对不对 —— 不参与被测程序本身, 也不依赖任何第三方库。
    """
    if n < 2:
        return 0, 0
    sieve = bytearray([1]) * (n + 1)
    sieve[0] = sieve[1] = 0
    i = 2
    while i * i <= n:
        if sieve[i]:
            step = i
            start = i * i
            sieve[start::step] = bytearray(len(range(start, n + 1, step)))
        i += 1
    count = sum(sieve)                              # 字节求和 = 计数(C 层, 快)
    total = sum(i for i in range(2, n + 1) if sieve[i])
    return count, total


def parse_answer(text: str):
    """取"最后一行 = 恰好两个整数"(约定: 个数 空格 和)。格式不符返回 None。

    故意严格: 判定要能一眼看出对错 —— 宽松地"从输出里抠两个数"会把 '4 and 17' 这类
    也算通过, 那验证就没有意义了。
    """
    lines = [ln.strip() for ln in text.strip().splitlines() if ln.strip()]
    if not lines:
        return None
    m = re.fullmatch(r"(-?\d+)\s+(-?\d+)", lines[-1])
    if not m:
        return None
    return int(m.group(1)), int(m.group(2))


def check(ws: str, program: str, n: int, target_sec: float):
    """判定一轮: 返回 (ok, 说明, 用时秒)。说明里带 ASCII 标记, 失败原因能直接当下一轮输入。"""
    path = os.path.join(ws, program)
    if not os.path.isfile(path):
        return False, (f"{FAIL_TAG} 工作区根目录下没有 {program}: 请写一个能这样跑的程序 ——"
                       f" `python {program} {n}` 打印一行两个整数: {n} 以内质数的**个数**与**之和**"
                       f"(空格分隔), 例: `python {program} 10` -> 4 17"), 0.0

    try:
        # utf-8-sig: 编辑器/Set-Content 写出来的 BOM 会让下面那条正则的 ^ 匹配不上
        src = open(path, "r", encoding="utf-8-sig", errors="replace").read()
    except OSError as e:
        return False, f"{FAIL_TAG} 读 {program} 失败: {e}", 0.0
    m = FORBIDDEN.search(src)
    if m:
        return False, (f"{FAIL_TAG} {program} 用了 {m.group(1)}(要求单线程: 只许普通单线程代码, "
                       f"不要线程/进程/协程池)"), 0.0

    expect = reference_primes(n)
    limit = max(3.0, target_sec * 3.0)
    t0 = time.perf_counter()
    try:
        r = subprocess.run([sys.executable, program, str(n)], cwd=ws,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=limit)
        text = r.stdout.decode("utf-8", "replace")
        rc = r.returncode
    except subprocess.TimeoutExpired:
        el = time.perf_counter() - t0
        return False, (f"{FAIL_TAG} `python {program} {n}` 跑过 {limit:.1f}s 还没出结果"
                       f"(要求 {target_sec}s 内算完): 换成筛法(埃氏筛/线性筛), 别对每个数试除"), el
    except OSError as e:
        return False, f"{FAIL_TAG} 起不了 `python {program} {n}`: {e}", time.perf_counter() - t0

    el = time.perf_counter() - t0
    if rc != 0:
        tail = "\n".join(text.strip().splitlines()[-8:])
        return False, f"{FAIL_TAG} 程序退出码 {rc}(用时 {el:.3f}s), 输出尾部:\n{tail}", el

    got = parse_answer(text)
    if got is None:
        return False, (f"{FAIL_TAG} 输出格式不对(用时 {el:.3f}s): 要求**一行**两个整数 ——"
                       f"个数 与 空格 和; 实际输出: {text.strip()[:120]!r}"), el
    if got != expect:
        wrong = []
        if got[0] != expect[0]:
            wrong.append(f"个数 期望 {expect[0]} 实际 {got[0]}")
        if got[1] != expect[1]:
            wrong.append(f"和 期望 {expect[1]} 实际 {got[1]}")
        return False, (f"{FAIL_TAG} N={n} 的结果不对: " + "; ".join(wrong) +
                       "(注意边界: 含 N 本身, 1 不是质数)"), el

    if el > target_sec:
        return False, (f"{FAIL_TAG} 结果对但太慢: {el:.3f}s > 上限 {target_sec}s(N={n})。"
                       f"下一步只做提速: 减少筛的重复标记/少分配/用 bytearray 的切片赋值"), el

    return True, f"{PASS_TAG} N={n} 结果正确, 用时 {el:.3f}s(上限 {target_sec}s)", el


def list_rounds(backup_dir: str) -> list:
    if not os.path.isdir(backup_dir):
        return []
    items = [d for d in os.listdir(backup_dir)
             if re.match(r"^round_\d+", d) and os.path.isdir(os.path.join(backup_dir, d))]
    return sorted(items)


def copy_program(ws: str, dest: str, backup_dir: str) -> int:
    """把这版程序整份复制到 dest(保持目录结构), 返回复制的文件数。

    只带走"程序文件"(见 PROGRAM_SUFFIX): 备份目录本身若在工作区里也跳过, 免得自己拷自己。
    """
    ws = os.path.abspath(ws)
    dest_abs = os.path.abspath(dest)
    backup_abs = os.path.abspath(backup_dir)
    n = 0
    for root, dirs, files in os.walk(ws):
        dirs[:] = [d for d in dirs
                   if d not in SKIP_DIRS and not d.startswith(".")
                   and os.path.abspath(os.path.join(root, d)) != dest_abs
                   and not os.path.abspath(os.path.join(root, d)).startswith(backup_abs + os.sep)]
        rel_dir = os.path.relpath(root, ws)
        for name in files:
            if name.startswith(".") or not name.endswith(PROGRAM_SUFFIX):
                continue
            rel = name if rel_dir == "." else os.path.join(rel_dir, name)
            target = os.path.join(dest, rel)
            os.makedirs(os.path.dirname(target), exist_ok=True)
            shutil.copy2(os.path.join(root, name), target)
            n += 1
    return n


def write_result(ws: str, data: dict) -> None:
    """把本轮结果写一份到工作区(verify_result.json): 下一轮的模型可以读它知道"上一轮多快"。"""
    import json
    try:
        with open(os.path.join(ws, RESULT_FILE), "w", encoding="utf-8") as f:
            json.dump(data, f, ensure_ascii=False, indent=2)
    except OSError:
        pass


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="质数筛任务的验证程序: 判定 -> 复制 -> 提交 -> 停止")
    p.add_argument("--ws", default="", help="工作区(默认: 当前目录 —— agent_cli 用它的 rootDir 当 cwd)")
    p.add_argument("--program", default="solve.py", help="被测程序(默认 solve.py, 相对工作区)")
    p.add_argument("--min-n", type=int, default=100000, help="每轮随机取的 N 下限(默认 100000)")
    p.add_argument("--max-n", type=int, default=300000, help="每轮随机取的 N 上限(默认 300000)")
    p.add_argument("--target-sec", type=float, default=1.0, help="运行用时上限, 秒(默认 1.0)")
    p.add_argument("--min-rounds", type=int, default=2, help="至少通过几轮才发 [loop-stop](默认 2)")
    p.add_argument("--backup-dir", default="", help="备份根目录(默认: $LOOP_BACKUP_DIR 或 <ws>/_backups)")
    p.add_argument("--check-only", action="store_true",
                   help="只判定: 不复制/不提交/不发 [loop-stop](给外部复验用)")
    return p


def main(argv=None) -> int:
    force_utf8_stdout()
    args = build_parser().parse_args(argv)
    ws = os.path.abspath(args.ws or os.getcwd())
    backup_dir = args.backup_dir or os.environ.get("LOOP_BACKUP_DIR") or os.path.join(ws, "_backups")

    n = random.randint(min(args.min_n, args.max_n), max(args.min_n, args.max_n))
    ok, msg, el = check(ws, args.program, n, args.target_sec)
    out(msg)
    if args.check_only:
        return 0 if ok else 1
    if not ok:
        write_result(ws, {"ok": False, "n": n, "elapsed": round(el, 4), "reason": msg})
        return 1

    # ---- 通过: 复制(备份) -> (够了就) 停止; 不碰 git, 见文件头说明 ----
    rnd = len(list_rounds(backup_dir)) + 1
    dest = os.path.join(backup_dir, "round_%03d" % rnd)
    files = copy_program(ws, dest, backup_dir)
    try:
        os.makedirs(backup_dir, exist_ok=True)
        with open(os.path.join(backup_dir, "rounds.log"), "a", encoding="utf-8") as f:
            f.write("%s round_%03d n=%d elapsed=%.3fs\n"
                    % (time.strftime("%Y-%m-%d %H:%M:%S"), rnd, n, el))
    except OSError as e:
        out("[verify-warn] 轮次记录写不进去: %s" % e)
    write_result(ws, {"ok": True, "round": rnd, "n": n, "elapsed": round(el, 4),
                      "target_sec": args.target_sec, "backup": dest})
    out("%s 第 %d 轮: 备份 %d 个文件 -> %s(用时 %.3fs)" % (PASS_TAG, rnd, files, dest, el))

    if rnd >= args.min_rounds:
        out("%s 已通过 %d 轮(>= %d 轮)且本轮 %.3fs <= %.3fs: 目标达成, 本轮之后结束循环"
            % (STOP_TAG, rnd, args.min_rounds, el, args.target_sec))
    else:
        out("%s 下一轮: 继续用 `python %s <N>` 验证, 上一轮(N=%d)%.3fs —— N 每轮随机, 目标: 结果依然正确"
            % (NEXT_TAG, args.program, n, el))
    return 0


if __name__ == "__main__":
    sys.exit(main())
