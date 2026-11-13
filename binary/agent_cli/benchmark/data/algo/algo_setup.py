#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
algo_setup：为算法用例把测试输入（按 --input 的题目 id 从同目录 inputs/<id>.json 读取）写进工程，
并注入“评测框架”：
  - 把 algo_harness/main.ts 复制为工程 src/main.ts（框架入口，模型禁止修改）；
  - 把 algo_harness/solution.ts 复制为工程 src/solution.ts（模型要实现的文件，可覆盖）。
框架负责读取 testdata.json、调用 solve、写出 result.json；模型只实现 solve。

注意：这里只写入“输入”（testdata.json），绝不写入“预期答案”——预期由 algo_verify 用参考实现
当场重算，防止 agent 抄袭。

只声明 --root（用 parse_known_args 容忍 --input/--data）。
"""
import argparse
import json
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
HARNESS_DIR = os.path.join(HERE, "algo_harness")


def _copy(src_rel, dst_root, dst_rel):
    src = os.path.join(HARNESS_DIR, src_rel)
    dst = os.path.join(dst_root, dst_rel)
    if not os.path.isfile(src):
        print("[algo_setup] 找不到 harness: %s" % src)
        return False
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copyfile(src, dst)
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True, help="用例工程根目录")
    ap.add_argument("--input", default="", help="题目 id（用于定位 inputs/<id>.json）")
    ap.add_argument("--data", default="")
    args, _ = ap.parse_known_args()

    root = os.path.normpath(args.root)
    pid = (args.input or "").strip()
    if not pid:
        print("[algo_setup] 缺少题目 id（--input）")
        return 1

    src = os.path.join(HERE, "inputs", pid + ".json")
    if not os.path.isfile(src):
        print("[algo_setup] 找不到测试输入: %s" % src)
        return 1
    try:
        with open(src, encoding="utf-8") as f:
            text = f.read()
    except Exception as e:
        print("[algo_setup] 读取测试输入失败: %s" % e)
        return 1

    p1 = os.path.join(root, "testdata.json")
    build_dir = os.path.join(root, "build")
    try:
        os.makedirs(build_dir, exist_ok=True)
    except Exception:
        pass
    p2 = os.path.join(build_dir, "testdata.json")

    try:
        with open(p1, "w", encoding="utf-8") as f:
            f.write(text)
        with open(p2, "w", encoding="utf-8") as f:
            f.write(text)
    except Exception as e:
        print("[algo_setup] 写入失败: %s" % e)
        return 1

    # 注入框架文件（覆盖 initProject 生成的占位 main.ts）
    if not _copy("main.ts", root, "src/main.ts"):
        return 1
    if not _copy("solution.ts", root, "src/solution.ts"):
        return 1

    print("[algo_setup] 已写入 testdata.json 并注入 main.ts/solution.ts (len=%d)" % len(text))
    return 0


if __name__ == "__main__":
    sys.exit(main())
