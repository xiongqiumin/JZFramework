#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
net_setup：为 jsapi 的 net_* 用例注入"评测框架"：
  - 把 net_harness/main.ts 复制为工程 src/main.ts（框架入口，模型禁止修改）；
  - 把 net_harness/solution.ts 复制为工程 src/solution.ts（模型要实现的文件，可覆盖）；
  - 把评测输入（--input，形如 "<文本> <端口>"）写到 _input.txt（工程根 与 build/ 各一份，
    兼容不同 cwd 假设）。
框架负责读取/解析输入、调用 solve、输出结果；模型只实现 solve。

仅声明 --root（用 parse_known_args 容忍宿主附带的 --input/--data）。
"""
import argparse
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
HARNESS_DIR = os.path.join(HERE, "net_harness")


def _copy(src_rel, dst_root, dst_rel):
    src = os.path.join(HARNESS_DIR, src_rel)
    dst = os.path.join(dst_root, dst_rel)
    if not os.path.isfile(src):
        print("[net_setup] 找不到 harness: %s" % src)
        return False
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copyfile(src, dst)
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True, help="用例工程根目录")
    ap.add_argument("--input", default="", help="评测注入的输入文本，形如 '<文本> <端口>'")
    ap.add_argument("--data", default="")
    args, _ = ap.parse_known_args()

    root = os.path.normpath(args.root)

    # 注入框架文件（覆盖 initProject 生成的占位 main.ts）
    if not _copy("main.ts", root, "src/main.ts"):
        return 1
    if not _copy("solution.ts", root, "src/solution.ts"):
        return 1

    # 写输入文件（工程根 + build/，运行时 cwd 为 build/）
    text = args.input or ""
    build_dir = os.path.join(root, "build")
    try:
        os.makedirs(build_dir, exist_ok=True)
    except Exception:
        pass
    for p in (os.path.join(root, "_input.txt"), os.path.join(build_dir, "_input.txt")):
        try:
            with open(p, "w", encoding="utf-8") as f:
                f.write(text)
        except Exception as e:
            print("[net_setup] 写入 _input.txt 失败: %s" % e)
            return 1

    print("[net_setup] 已注入 main.ts/solution.ts 并写入输入(len=%d)" % len(text))
    return 0


if __name__ == "__main__":
    sys.exit(main())
