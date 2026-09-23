#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
fs_setup 准备脚本：为 jsapi_core_fs 用例制备固件——在工程 build/ 下建 sandbox/ 目录，
并放入 hello.txt（内容 "jzframework"）作为 agent 要读取/处理的“输入数据”。

仅声明 --root（用 parse_known_args 容忍宿主附带的 --input/--data）。agent 以 build/ 为 cwd 运行，
所以固件放在 build/sandbox/ 下。
"""
import argparse
import os
import sys


FIXTURE_TEXT = "jzframework"


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
    sandbox = os.path.join(build_dir, "sandbox")
    os.makedirs(sandbox, exist_ok=True)
    hello = os.path.join(sandbox, "hello.txt")
    try:
        with open(hello, "w", encoding="utf-8") as f:
            f.write(FIXTURE_TEXT)
    except Exception as e:
        print("[fs_setup] 写入固件失败: %s" % e)
        return 1

    print("[fs_setup] 已制备固件: %s (内容=%r)" % (hello, FIXTURE_TEXT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
