#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
打包 JZ JS 引擎产物, 供部署(拷到 exe 同配置目录 bin/<CONFIG>/)。

产出(统一放进一个大的部署目录 jz_engine/ 下):
  <out>/jz_engine/jz_types/ : 完整类型定义(JZJSEngine/types), 供 initProject 拷进用户工程
  <out>/jz_engine/jz_js/    : 运行时 JS(dist 编译产物), 含 core/net/serialport 的 dist/index.js

删除策略(重要):
  本脚本只会清理"它自己要写入的目标子目录"——即 <out>/jz_engine/jz_types 与
  <out>/jz_engine/jz_js。除此之外(父目录 jz_engine、<out>、以及 <out> 下的其它任何文件)
  一律不碰。删前用 _safe_clean 做路径守卫, 防止误删脚本输出范围之外的内容。
  这样既能避免旧 CMake 把 types 的 core/ 子集也拷进 jz_types 造成的 TS2300 残留冲突,
  又不会动到部署目录里其它东西(要手动清理其它文件请自行处理)。

依赖: dist 由 `tsc -b js_wrapper` 生成(非 CMake target)。某包 dist 缺失时跳过并告警。
"""
import argparse
import os
import shutil
import sys

# js_wrapper 下各运行时包(其编译产物位于 <pkg>/dist/index.js)
PACKAGES = ["core", "net", "serialport"]
# 统一部署目录名(放在 --out 之下)
BUNDLE = "jz_engine"
# 仅允许清理的脚本自有输出子目录(相对 BUNDLE)
CLEAN_TARGETS = ["jz_types", "jz_js"]


def _safe_clean(path, bundle_root):
    """只删除 bundle 内的脚本自有输出目录(jz_types/jz_js)及其子路径; 其余一律拒绝。"""
    ap = os.path.abspath(path)
    br = os.path.abspath(bundle_root)
    if ap != br and not ap.startswith(br + os.sep):
        raise RuntimeError(f"[package.py] REFUSE to delete outside bundle: {path}")
    allowed = False
    for t in CLEAN_TARGETS:
        tdir = os.path.join(br, t)
        if ap == tdir or ap.startswith(tdir + os.sep):
            allowed = True
            break
    if not allowed:
        raise RuntimeError(f"[package.py] REFUSE to delete non-target dir: {path}")
    if os.path.exists(ap):
        shutil.rmtree(ap)


def copy_tree_clean(src, dst, label, bundle_root):
    """清理目标子目录(仅限脚本自有输出)后拷贝 src -> dst, 避免旧文件残留。"""
    if not os.path.isdir(src):
        print(f"[package.py] WARN skip {label}: source not found: {src} "
              f"(need `tsc -b js_wrapper` first?)")
        return False
    _safe_clean(dst, bundle_root)
    shutil.copytree(src, dst)
    print(f"[package.py] {label}: {src} -> {dst}")
    return True


def main():
    ap = argparse.ArgumentParser(description="Package JZ JS engine artifacts.")
    ap.add_argument("--out", required=True,
                    help="output base dir, e.g. <build>/binary/code_agent/bin/<CONFIG>")
    ap.add_argument("--types", help="JZJSEngine/types source dir")
    ap.add_argument("--js-wrapper", help="JZJSEngine/js_wrapper source dir (contains <pkg>/dist)")
    args = ap.parse_args()

    out = os.path.abspath(args.out)
    bundle = os.path.join(out, BUNDLE)
    os.makedirs(bundle, exist_ok=True)   # 只创建, 绝不删除 bundle 本身或 <out>

    # 类型定义 -> jz_engine/jz_types
    if args.types:
        copy_tree_clean(os.path.abspath(args.types), os.path.join(bundle, "jz_types"),
                        "types", bundle)

    # 运行时 JS(dist) -> jz_engine/jz_js/<pkg>/dist, 保持兄弟布局供包间相对 import
    if args.js_wrapper:
        wrap = os.path.abspath(args.js_wrapper)
        any_ok = False
        for pkg in PACKAGES:
            src = os.path.join(wrap, pkg, "dist")
            if copy_tree_clean(src, os.path.join(bundle, "jz_js", pkg, "dist"),
                               f"dist:{pkg}", bundle):
                any_ok = True
        if not any_ok:
            print("[package.py] WARN: no dist packages found; runtime JS not packaged")

    print(f"[package.py] done -> {bundle}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
