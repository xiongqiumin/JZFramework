#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
setup 准备脚本（每个用例 initProject 之后、agent 开始前由宿主同步调用）。

只声明 --root：负责“准备数据/固件”的通用部分（确保 build/ 目录存在）。
各用例若有专属固件制备（如 fs_setup.py），由自己的 setup 脚本处理。

使用 parse_known_args 容忍宿主附带的 --input / --data（部分用例仍需要，由对应脚本处理）。
"""
import argparse
import os
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True, help="用例工程根目录")
    args, _ = ap.parse_known_args()

    root = os.path.normpath(args.root)
    build_dir = os.path.join(root, "build")
    try:
        os.makedirs(build_dir, exist_ok=True)
    except Exception:
        pass

    print("[setup] 已就绪: %s" % root)
    return 0


if __name__ == "__main__":
    sys.exit(main())
