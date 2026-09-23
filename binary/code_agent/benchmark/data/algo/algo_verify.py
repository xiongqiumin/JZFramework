#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
algo_verify：启动模型产物（code_runtime, cwd=build），读取 agent 写出的 ./result.json，
并与“参考实现”当场重算的正确输出逐项精确比对。预期答案从不落地，杜绝 agent 抄袭。

退出码：0=通过，1=未通过（stdout 作为模型重试反馈）。
"""
import argparse
import json
import os
import subprocess
import sys


# ---- 参考实现（用于重算正确输出）----
def ref_bubble_sort(a):
    return sorted(a)


def ref_reverse_array(a):
    return list(reversed(a))


def ref_binary_search(inp):
    arr, t = inp[0], inp[1]
    try:
        return arr.index(t)
    except ValueError:
        return -1


def ref_two_sum(inp):
    nums, target = inp[0], inp[1]
    n = len(nums)
    for i in range(n):
        for j in range(i + 1, n):
            if nums[i] + nums[j] == target:
                return [i, j]
    return [-1, -1]


def ref_fib(n):
    a, b = 0, 1
    for _ in range(n):
        a, b = b, a + b
    return a


def ref_is_palindrome(s):
    return s == s[::-1]


def ref_max_subarray(a):
    if not a:
        return 0
    best = cur = a[0]
    for x in a[1:]:
        cur = max(x, cur + x)
        best = max(best, cur)
    return best


def ref_gcd(inp):
    a, b = inp[0], inp[1]
    a, b = abs(a), abs(b)
    while b:
        a, b = b, a % b
    return a


REFERENCE = {
    "bubble_sort": ref_bubble_sort,
    "reverse_array": ref_reverse_array,
    "binary_search": ref_binary_search,
    "two_sum": ref_two_sum,
    "fib": ref_fib,
    "is_palindrome": ref_is_palindrome,
    "max_subarray": ref_max_subarray,
    "gcd": ref_gcd,
}

TIMEOUT = 20


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runtime", required=True, help="code_runtime 可执行文件路径")
    ap.add_argument("--build", required=True, help="模型产物构建目录（agent 运行 cwd）")
    ap.add_argument("--root", default="")
    ap.add_argument("--problem", required=True, help="题目 id")
    args = ap.parse_args()

    build = os.path.normpath(args.build)
    ref = REFERENCE.get(args.problem)
    if ref is None:
        print("未知 problem: %s" % args.problem)
        return 1

    td_path = os.path.join(build, "testdata.json")
    if not os.path.isfile(td_path):
        print("[FAIL] 找不到 testdata.json: %s" % td_path)
        return 1
    try:
        with open(td_path, encoding="utf-8") as f:
            td = json.load(f)
    except Exception as e:
        print("[FAIL] testdata.json 解析失败: %s" % e)
        return 1
    inputs = td.get("inputs", [])

    # ---- 启动模型产物（做完算法计算即退出）----
    try:
        proc = subprocess.Popen([args.runtime], cwd=build,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    except Exception as e:
        print("无法启动 code_runtime: %s" % e)
        return 1
    try:
        out, _ = proc.communicate(timeout=TIMEOUT)
    except Exception:
        proc.kill()
        print("code_runtime 超时(%ds)" % TIMEOUT)
        return 1
    js_out = out.decode("utf-8", "replace") if out else ""
    if proc.returncode != 0:
        print("[CRASH] code_runtime 退出码 %d" % proc.returncode)
        if js_out.strip():
            print(js_out)
        return 1

    res_path = os.path.join(build, "result.json")
    if not os.path.isfile(res_path):
        print("[FAIL] 找不到 result.json（agent 未写出结果）")
        if js_out.strip():
            print(js_out)
        return 1
    try:
        with open(res_path, encoding="utf-8") as f:
            res = json.load(f)
    except Exception as e:
        print("[FAIL] result.json 解析失败: %s" % e)
        return 1
    outputs = res.get("outputs", None)
    if not isinstance(outputs, list) or len(outputs) != len(inputs):
        print("[FAIL] outputs 缺失/长度不符：期望 %d 实际 %s"
              % (len(inputs), type(outputs).__name__))
        if js_out.strip():
            print(js_out)
        return 1

    failed = []
    for i, (inp, got) in enumerate(zip(inputs, outputs)):
        exp = ref(inp)
        if got != exp:
            failed.append((i, inp, exp, got))
    if failed:
        print("RESULT: FAIL (%d/%d 用例不符)" % (len(failed), len(inputs)))
        for i, inp, exp, got in failed[:10]:
            print("  用例#%d 输入=%r 期望=%r 实际=%r" % (i, inp, exp, got))
        if js_out.strip():
            print(js_out)
        return 1
    print("RESULT: PASS (%d 用例全部通过)" % len(inputs))
    return 0


if __name__ == "__main__":
    sys.exit(main())
