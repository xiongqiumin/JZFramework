#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
通用验证程序：编译 TS 工程 -> 按 tsconfig 的配置定位产物目录 -> 用 code_runtime 跑产物
-> 比对它的输出是否符合预期。

它是 benchmark 宿主里"原本内联在 main.cpp"的那段判定（tsc 编译 + 跑 code_runtime +
check(output)，判定条件来自用例 JSON 的 expect.contains）拆出来的独立脚本，好处是别的
驱动端（agent_cli_test 等）也能用同一条判定链路，不必各写一套。

所有路径都由外部传入，脚本自己**不做任何目录搜索**：
  --root  工程根（固定），tsconfig.json 必须在这里；
  --tsc   tsc 可执行文件全路径；
  --runtime  code_runtime 可执行文件全路径；
产物目录（运行根）默认按 tsconfig 的 compilerOptions.outDir 解析（相对 --root 展开），
也可以用 --build 显式覆盖。

用法：
  python stdout_verify.py --tsc <tsc> --runtime <code_runtime> --root <工程根> \
      --contains "hello world" [--contains ...] [--case-insensitive] \
      [--not-contains ...] [--timeout 30] [--build-timeout 120] [--build <产物目录>]

步骤（每一步失败都直接退 1，并把实际输出打出来当重试反馈）：
  1) 编译：`tsc -p <root>/tsconfig.json`（cwd = 工程根），非 0 退出/超时 -> 未通过；
  2) 读 tsconfig（JSONC，容忍注释与尾逗号）拿 compilerOptions.outDir / rootDir，
     并把它打印出来（"根据配置去运行"，配置改了这里也能看出来）；
  3) 检查产物目录里**真的编译出了 JS**：必须有入口 main.js，并列出实际产出的 .js 清单
     （编译"成功"却没产物、或入口名不对，都会在这一步被抓到）；
  4) 以产物目录为工作目录启动 code_runtime（并把同一个目录显式传给 --root）；
  5) 判定：产物正常退出(0)且全部 --contains 命中 -> 通过；
     产物异常退出 -> 未通过；产物在 --timeout 内没退出(常驻/GUI) -> 强制结束后按已捕获输出判定。

退出码：0 = 通过；1 = 未通过（stdout 即给模型的重试反馈）。
"""

import argparse
import json
import locale
import os
import subprocess
import sys

MAX_PRINT_CHARS = 8000


def fix_stdout():
    # 验证输出会经 agent_cli 的 JSON 回包(UTF-8)出去, 也可能直接打到控制台:
    # 明确写 UTF-8, 且遇到不可编码字符不炸(否则 Python 自己会抛 UnicodeEncodeError)。
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass


def decode(data):
    """子进程输出按"先 UTF-8, 再本机 ANSI(cp936 等)"尝试解码, 都不行就替换不可解字符。"""
    if not data:
        return ""
    encs = ["utf-8"]
    try:
        pref = locale.getpreferredencoding(False)
        if pref and pref.lower().replace("-", "") not in ("utf8", "utf8mb4"):
            encs.append(pref)
    except Exception:
        pass
    for e in encs:
        try:
            return data.decode(e)
        except Exception:
            pass
    return data.decode("utf-8", "replace")


def run_cmd(argv, cwd, timeout):
    """跑一个子进程(合并 stderr)。返回 (started, exitCode, text, timedOut)。"""
    try:
        proc = subprocess.Popen(argv, cwd=cwd,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    except Exception as e:
        return False, -1, "无法启动 %s: %s" % (argv[0], e), False

    timed_out = False
    try:
        raw, _ = proc.communicate(timeout=(timeout if timeout > 0 else None))
    except subprocess.TimeoutExpired:
        timed_out = True
        proc.kill()
        try:
            raw, _ = proc.communicate(timeout=5)
        except Exception:
            raw = b""
    return True, proc.returncode, decode(raw), timed_out


def strip_jsonc(text):
    """tsconfig.json 是 JSONC：剥掉 // 与 /* */ 注释、去掉对象/数组末项的尾逗号。"""
    # 第一遍: 去掉注释(字符串里的 / 不动)
    out = []
    i, n = 0, len(text)
    in_str = False
    while i < n:
        c = text[i]
        if in_str:
            out.append(c)
            if c == "\\" and i + 1 < n:
                out.append(text[i + 1])
                i += 2
                continue
            if c == '"':
                in_str = False
            i += 1
            continue
        if c == '"':
            in_str = True
            out.append(c)
            i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] not in "\r\n":
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                i += 1
            i += 2
            continue
        out.append(c)
        i += 1

    # 第二遍: 去掉尾逗号(后面只有空白再接 } 或 ] 的逗号)
    s = "".join(out)
    res = []
    i, n = 0, len(s)
    in_str = False
    while i < n:
        c = s[i]
        if in_str:
            res.append(c)
            if c == "\\" and i + 1 < n:
                res.append(s[i + 1])
                i += 2
                continue
            if c == '"':
                in_str = False
            i += 1
            continue
        if c == '"':
            in_str = True
            res.append(c)
            i += 1
            continue
        if c == ",":
            j = i + 1
            while j < n and s[j] in " \t\r\n":
                j += 1
            if j < n and s[j] in "}]":
                i += 1
                continue     # 尾逗号: 丢掉
        res.append(c)
        i += 1
    return "".join(res)


def read_tsconfig(tsconfig_path):
    with open(tsconfig_path, "rb") as f:
        raw = f.read()
    if raw[:3] == b"\xef\xbb\xbf":     # 去 BOM
        raw = raw[3:]
    text = raw.decode("utf-8", "replace")
    return json.loads(strip_jsonc(text))


def abs_from_root(root, p):
    if not p:
        return root
    if os.path.isabs(p):
        return os.path.normpath(p)
    return os.path.normpath(os.path.join(root, p))


def list_emitted_js(prod_dir):
    """产物目录下所有 .js（相对路径）, 用于证明"确实编译出 JS 了"。"""
    found = []
    for cur, dirs, files in os.walk(prod_dir):
        dirs[:] = [d for d in dirs if d != "node_modules"]
        for f in files:
            if f.endswith(".js"):
                found.append(os.path.relpath(os.path.join(cur, f), prod_dir).replace("\\", "/"))
    found.sort()
    return found


def main():
    fix_stdout()

    ap = argparse.ArgumentParser(
        description="通用验证: tsc 编译 -> 按 tsconfig 定位产物 -> code_runtime 跑 + 比对输出")
    ap.add_argument("--root", required=True, help="工程根目录(tsconfig.json 所在目录)")
    ap.add_argument("--tsc", required=True, help="tsc 可执行文件全路径")
    ap.add_argument("--runtime", required=True, help="code_runtime 可执行文件全路径")
    ap.add_argument("--build", default="",
                    help="产物目录(运行根); 缺省按 tsconfig 的 compilerOptions.outDir 解析")
    ap.add_argument("--contains", action="append", default=[],
                    help="输出里必须出现的文本(可给多次, 全部命中才算通过)")
    ap.add_argument("--not-contains", action="append", default=[],
                    help="输出里不允许出现的文本(可给多次)")
    ap.add_argument("--case-insensitive", action="store_true",
                    help="contains / not-contains 比较忽略大小写")
    ap.add_argument("--timeout", type=int, default=30,
                    help="产物运行上限(秒), 0=不限; 超时强制结束并按已捕获输出判定")
    ap.add_argument("--build-timeout", type=int, default=120, help="编译上限(秒)")
    args = ap.parse_args()

    root = os.path.normpath(args.root)
    tsconfig_path = os.path.join(root, "tsconfig.json")

    # ---- 0) 前置条件(全部由外部传入, 不搜索) ----
    if not os.path.isdir(root):
        print("[FAIL] 工程根不存在: %s" % root)
        return 1
    if not os.path.isfile(tsconfig_path):
        print("[FAIL] 不是 TS 工程(缺少 tsconfig.json): %s" % tsconfig_path)
        return 1
    for label, p in (("tsc", args.tsc), ("code_runtime", args.runtime)):
        if not os.path.isfile(p):
            print("[FAIL] %s 不存在: %s" % (label, p))
            return 1

    # ---- 1) 先编译（命令行与宿主 buildProject 一致: tsc -p tsconfig.json --pretty false） ----
    print("[build] %s -p %s --pretty false" % (args.tsc, tsconfig_path))
    started, rc, out, timed = run_cmd([args.tsc, "-p", tsconfig_path, "--pretty", "false"],
                                      root, args.build_timeout)
    if not started:
        print("[FAIL] " + out)
        return 1
    if timed:
        print("[FAIL] 编译超时(%ds)" % args.build_timeout)
        if out.strip():
            print(out)
        return 1
    if rc != 0:
        print("[FAIL] 编译失败(tsc 退出码 %d)" % rc)
        print(out if out.strip() else "(tsc 无输出)")
        return 1
    print("[PASS] 编译通过")
    if out.strip():
        print("[note] tsc 输出:")
        print(out)

    # ---- 2) 检测 TS 配置, 按配置定位产物目录 ----
    try:
        cfg = read_tsconfig(tsconfig_path)
    except Exception as e:
        print("[FAIL] tsconfig.json 解析失败: %s" % e)
        return 1
    co = cfg.get("compilerOptions") if isinstance(cfg, dict) else None
    if not isinstance(co, dict):
        print("[FAIL] tsconfig.json 里没有 compilerOptions 对象")
        return 1
    out_dir = co.get("outDir") or "build"
    root_dir = co.get("rootDir") or "src"
    print("[cfg] tsconfig: outDir=%s rootDir=%s" % (out_dir, root_dir))

    prod = abs_from_root(root, args.build or out_dir)
    print("[cfg] 产物目录(运行根): %s%s" % (prod, "  [--build 覆盖]" if args.build else ""))
    if not os.path.isdir(prod):
        print("[FAIL] 产物目录不存在: %s" % prod)
        print("  检查 tsconfig 的 outDir 与 src 下是否有可编译的 .ts(rootDir=%s)" % root_dir)
        return 1

    # ---- 3) 检查是不是真的编译出了 JS(尤其入口 main.js) ----
    emitted = list_emitted_js(prod)
    print("[check] 产物目录里的 JS: %d 个%s"
          % (len(emitted), (" (" + ", ".join(emitted[:6]) + ("..." if len(emitted) > 6 else "") + ")")
             if emitted else ""))
    entry = os.path.join(prod, "main.js")
    if not os.path.isfile(entry):
        print("[FAIL] 没有编译出入口 main.js: %s" % entry)
        print("  产物目录内容: %s" % (", ".join(sorted(os.listdir(prod))[:20]) or "(空)"))
        print("  检查: src 下是否有入口 main.ts(tsconfig rootDir=%s outDir=%s)" % (root_dir, out_dir))
        return 1
    print("[PASS] 入口 main.js 已生成")

    # ---- 4) 跑产物 ----
    print("[run] %s --root %s" % (args.runtime, prod))
    started, rc, output, timed_out = run_cmd([args.runtime, "--root", prod], prod, args.timeout)
    if not started:
        print("[FAIL] " + output)
        return 1

    # ---- 5) 判定 ----
    note = []
    hard_fail = []
    if timed_out:
        note.append("产物在 %ds 内未退出(常驻/GUI?), 已强制结束 —— 按已捕获输出判定" % args.timeout)
    else:
        print("[run] 产物退出码: %s" % rc)
        if rc != 0:
            hard_fail.append("产物异常退出(退出码 %s)" % rc)

    checks = []
    hay = output.lower() if args.case_insensitive else output
    for c in args.contains:
        checks.append(("输出包含 \"%s\"" % c,
                       (c.lower() if args.case_insensitive else c) in hay))
    for c in args.not_contains:
        checks.append(("输出不包含 \"%s\"" % c,
                       (c.lower() if args.case_insensitive else c) not in hay))
    if not checks and not hard_fail and timed_out:
        hard_fail.append("产物既没退出、也没有可判定的断言: 无法确认产物是否正常")

    failed = [n for (n, ok) in checks if not ok]

    if not timed_out and rc == 0:
        print("[PASS] 产物正常退出")
    for (n, ok) in checks:
        print("[%s] %s" % ("PASS" if ok else "FAIL", n))
    for f in hard_fail:
        print("[FAIL] " + f)
    for n in note:
        print("[note] " + n)

    passed = (not hard_fail) and (not failed)

    if not passed:
        print("--- 产物实际输出 ---")
        text = output if output.strip() else "(无输出)"
        if len(text) > MAX_PRINT_CHARS:
            text = text[:MAX_PRINT_CHARS] + "\n...(输出已截断, 共 %d 字符)" % len(output)
        print(text)
        total = len(checks) + len(hard_fail)
        if total == 0:
            print("RESULT: FAIL (无有效断言)")
        else:
            print("RESULT: FAIL (%d/%d 未通过)" % (len(failed) + len(hard_fail), total))
        return 1

    print("RESULT: PASS (%d 断言全部通过)" % len(checks) if checks
          else "RESULT: PASS (产物正常退出, 无断言)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
