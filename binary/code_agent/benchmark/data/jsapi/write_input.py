#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
write_input 准备脚本：把评测注入的用例输入（--input）写到工程里，让 agent 直接读本地文件
（./_input.txt），而不必走 HTTP /input。写入两处（工程根 与 build/）以兼容不同 cwd 假设。

仅“仍需本地输入文件”的用例使用；新设计的用例请改用专属 setup + verify（如 fs_setup.py / fs_verify.py）。
"""
import argparse
import os
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True, help="用例工程根目录")
    ap.add_argument("--input", default="", help="评测注入的输入文本")
    ap.add_argument("--data", default="", help="评测集目录(本脚本未使用, 预留)")
    args, _ = ap.parse_known_args()

    root = os.path.normpath(args.root)
    text = args.input

    p1 = os.path.join(root, "_input.txt")
    build_dir = os.path.join(root, "build")
    try:
        os.makedirs(build_dir, exist_ok=True)
    except Exception:
        pass
    p2 = os.path.join(build_dir, "_input.txt")

    try:
        with open(p1, "w", encoding="utf-8") as f:
            f.write(text)
        with open(p2, "w", encoding="utf-8") as f:
            f.write(text)
    except Exception as e:
        print("[write_input] 写入失败: %s" % e)
        return 1

    print("[write_input] 已写入输入: %s (len=%d)" % (p1, len(text)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
