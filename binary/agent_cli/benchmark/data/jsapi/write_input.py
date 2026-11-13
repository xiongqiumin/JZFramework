#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
write_input 准备脚本：把评测注入的用例输入（--input）落地成文件。

· 给了 --input-dir（agent_cli_test 走这条）：只写到**工程之外**的输入目录 <input-dir>/input.txt。
  工程根里不出现任何输入文件 —— 模型删不掉，也不会顺着 `..` 翻到别的用例的输入；
  且这个目录在工作区之外，属于"区外只读"，模型改不了。
· 没给（benchmark 宿主）：保持老行为，工程根 与 build/ 各写一份。

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
    ap.add_argument("--input-dir", default="", help="输入目录(工程之外)：给了就只写这里")
    args, _ = ap.parse_known_args()

    root = os.path.normpath(args.root)
    text = args.input

    targets = []
    if args.input_dir:
        input_dir = os.path.normpath(args.input_dir)
        try:
            os.makedirs(input_dir, exist_ok=True)
        except Exception as e:
            print("[write_input] 创建输入目录失败: %s" % e)
            return 1
        targets.append(os.path.join(input_dir, "input.txt"))
    else:
        build_dir = os.path.join(root, "build")
        try:
            os.makedirs(build_dir, exist_ok=True)
        except Exception:
            pass
        targets.extend([os.path.join(root, "_input.txt"),
                        os.path.join(build_dir, "_input.txt")])

    try:
        for p in targets:
            with open(p, "w", encoding="utf-8") as f:
                f.write(text)
    except Exception as e:
        print("[write_input] 写入失败: %s" % e)
        return 1

    print("[write_input] 已写入输入: %s (len=%d)" % (targets[0], len(text)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
