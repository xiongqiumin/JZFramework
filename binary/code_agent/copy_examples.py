#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
把 runtime 下的示例工程**只拷源码**铺到发布目录的 example/ 下（随发布一起分发）。

用法（由 binary/CMakeLists.txt 的 copy_examples 目标调用）:

  python copy_examples.py --src <repo>/binary/code_agent/runtime --dst <exe>/example

做的事:

  ① 逐个示例整棵拷过去，但跳过**编译产物与依赖缓存**：
       build/（tsconfig 的 outDir）、node_modules/、*.tsbuildinfo、*.js.map
     —— 示例是给人读/给人改的源码，跟着它们自己编译出来的东西走没意义；
  ② 每个 tsconfig.json 里那条"类型来源"是**仓库布局的相对路径**
     （../../../../JZJSEngine/js_wrapper/...），在部署目录里解析不到 ⇒ 改写成部署布局的
       ../../../../JZJSEngine/js_wrapper/  ->  ../../js_wrapper/
     （示例在 <exe>/example/<名>/，共享的类型/运行时树在同级 <exe>/js_wrapper/），
     并在文件头插一行"这是部署副本"的说明。**仓库里那份源码不动**。

删除策略: 本脚本的输出就是 --dst 那一棵，完全由它生成 ⇒ 每次先整棵删掉再重铺；
删前做守卫（只允许删名字 = example 的目录本身，绝不碰它的父目录）。
"""
import argparse
import os
import shutil
import sys

# 随发布一起走的示例（目录名 = 源目录名）；新增一个就往这里加名字。
EXAMPLES = ["jsapi_example", "test_jswidget", "test_jsshooter", "bteserver", "gomoku"]

# 不铺的目录 / 后缀（示例自己的编译产物与依赖缓存）
SKIP_DIRS = {"build", "node_modules", ".git", "__pycache__"}
SKIP_SUFFIXES = (".tsbuildinfo", ".js.map")

# tsconfig 里"类型来源"的仓库相对前缀（见各示例 tsconfig 的 include）
REPO_TYPES_PREFIX = "../../../../JZJSEngine/js_wrapper/"
# 部署布局下等价的前缀：示例在 <exe>/example/<名>/，共享树在 <exe>/js_wrapper/
DEPLOY_TYPES_PREFIX = "../../js_wrapper/"

DEPLOY_NOTE = (
    "// ⚠ 本文件是**部署副本**（由 binary/code_agent/copy_examples.py 生成）：\n"
    "//   include 里的类型来源已从仓库相对路径改写为部署布局的 ../../js_wrapper/...\n"
    "//   （示例在 <exe>/example/<名>/，共享的类型/运行时树在同级 <exe>/js_wrapper/）。\n"
)


def _ignore(dir_path, names):
    """copytree 的 ignore 回调：跳过编译产物 / 依赖缓存。"""
    drop = []
    for n in names:
        if n in SKIP_DIRS or n.endswith(SKIP_SUFFIXES):
            drop.append(n)
    return set(drop)


def _clean_dst(dst):
    """整棵删掉上次的部署目录（守卫：必须是名为 example 的目录本身）。"""
    ap = os.path.abspath(dst)
    if os.path.basename(ap) != "example":
        raise RuntimeError(f"[copy_examples] REFUSE to delete non-example dir: {ap}")
    if os.path.isdir(ap):
        shutil.rmtree(ap)


def _rewrite_tsconfig(path, name):
    """把部署副本的 tsconfig 里仓库相对类型路径改写成部署布局的（返回替换行数）。"""
    with open(path, encoding="utf-8-sig") as f:
        text = f.read()
    n = text.count(REPO_TYPES_PREFIX)
    if n == 0:
        print(f"[copy_examples] WARN {name}/tsconfig.json: "
              f"没找到类型来源前缀 {REPO_TYPES_PREFIX}（示例改了 tsconfig？）")
        return 0
    text = DEPLOY_NOTE + text.replace(REPO_TYPES_PREFIX, DEPLOY_TYPES_PREFIX)
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)
    print(f"[copy_examples] {name}/tsconfig.json: {n} 处类型路径 -> {DEPLOY_TYPES_PREFIX}")
    return n


def main():
    ap = argparse.ArgumentParser(description="Deploy JZ example projects (source only).")
    ap.add_argument("--src", required=True,
                    help="runtime 目录，例如 <repo>/binary/code_agent/runtime")
    ap.add_argument("--dst", required=True,
                    help="发布目录下的 example 目录，例如 <exe>/example")
    args = ap.parse_args()

    src = os.path.abspath(args.src)
    dst = os.path.abspath(args.dst)
    if not os.path.isdir(src):
        print(f"[copy_examples] ERROR: --src not found: {src}", file=sys.stderr)
        return 2

    _clean_dst(dst)
    os.makedirs(dst, exist_ok=True)

    missing = 0
    for name in EXAMPLES:
        s = os.path.join(src, name)
        if not os.path.isdir(s):
            print(f"[copy_examples] WARN skip {name}: source not found: {s}")
            missing += 1
            continue
        d = os.path.join(dst, name)
        shutil.copytree(s, d, ignore=_ignore)
        print(f"[copy_examples] {name}: {s} -> {d}")
        ts = os.path.join(d, "tsconfig.json")
        if os.path.isfile(ts):
            _rewrite_tsconfig(ts, name)

    print(f"[copy_examples] done -> {dst}")
    return 0 if missing == 0 else 0


if __name__ == "__main__":
    sys.exit(main())
