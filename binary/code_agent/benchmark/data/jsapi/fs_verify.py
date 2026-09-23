#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
fs_verify 校验脚本：启动模型产物（code_runtime, cwd=build），等待其退出，
然后直接检查文件系统副作用，验证 agent 真的会用 fs 模块做：读文件 / 递归建目录 / 写文件 / 追加（不覆盖）。

评测任务（见 core_fs.json prompt）：
  - 读 build/sandbox/hello.txt（内容 "jzframework"）记为 T；
  - 在 build/sandbox/ 下递归创建 a/b/c/ 目录；
  - 把 T 反转后写入 build/sandbox/a/b/c/rev.txt（不加换行）；
  - 在该文件末尾追加 "OK"（不加换行）。
期望：build/sandbox/a/b/c/rev.txt 内容 == "krowemarfzj" + "OK" == "krowemarfzjOK"，且 a/b/c 目录存在。

退出码：0=通过，1=未通过（stdout 作为模型重试反馈）。
"""
import argparse
import os
import subprocess
import sys


EXPECTED = "krowemarfzjOK"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runtime", required=True, help="code_runtime 可执行文件路径")
    ap.add_argument("--build", required=True, help="模型产物构建目录（agent 运行 cwd）")
    ap.add_argument("--root", default="")
    args = ap.parse_args()

    build = os.path.normpath(args.build)

    # ---- 启动模型产物（one-shot：做完 fs 操作即退出）----
    try:
        proc = subprocess.Popen([args.runtime], cwd=build,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    except Exception as e:
        print("无法启动 code_runtime: %s" % e)
        return 1

    try:
        out, _ = proc.communicate(timeout=20)
    except Exception:
        proc.kill()
        print("code_runtime 超时(20s)")
        return 1
    js_out = out.decode("utf-8", "replace") if out else ""

    if proc.returncode != 0:
        print("[CRASH] code_runtime 退出码 %d" % proc.returncode)
        if js_out.strip():
            print(js_out)
        return 1

    results = []

    def check(name, ok, detail=""):
        results.append((name, ok, detail))
        tag = "PASS" if ok else "FAIL"
        print("[%s] %s%s" % (tag, name, (": " + detail) if detail else ""))

    out_dir = os.path.join(build, "sandbox", "a", "b", "c")
    rev_file = os.path.join(out_dir, "rev.txt")

    # 1) 递归目录 a/b/c 真的被创建
    check("mkdir_recursive", os.path.isdir(out_dir), out_dir)

    # 2) rev.txt 存在且内容正确（读 + 写 + 追加三段都做对）
    if os.path.isfile(rev_file):
        try:
            with open(rev_file, "r", encoding="utf-8") as f:
                content = f.read()
            check("rev_content", content == EXPECTED,
                  "期望=%r 实际=%r" % (EXPECTED, content))
        except Exception as e:
            check("rev_read", False, str(e))
    else:
        check("rev_file_exists", False, rev_file)

    failed = [r for r in results if not r[1]]
    if not results:
        print("RESULT: FAIL (无有效断言)")
        return 1
    if failed:
        print("RESULT: FAIL (%d/%d 失败)" % (len(failed), len(results)))
        if js_out.strip():
            print(js_out)
        return 1
    print("RESULT: PASS (%d 断言全部通过)" % len(results))
    return 0


if __name__ == "__main__":
    sys.exit(main())
