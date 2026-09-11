#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
打包 JZ JS 引擎产物, 供部署(拷到 exe 同配置目录 bin/<CONFIG>/)。
产出(统一放进一个大的部署目录 jz_engine/ 下):
  <out>/jz_engine/jz_types/ : 完整类型定义, 供 initProject 拷进用户工程
  <out>/jz_engine/jz_js/    : 运行时 JS(dist 编译产物), 含各包 dist/index.js

类型来源(单一来源 = js_wrapper 自身, 已不再有 JZJSEngine/types):
  <wrap>/types/          : 共享声明 —— 全局类/console/fetch/EventEmitter 等 + 各模块的
                           ambient 转发 shim(declare module 'fs' ...), 供 import type 使用
  <wrap>/<pkg>/types/    : 各包类型(模块形式, 与包内实现同源)

部署布局(与源码 js_wrapper/ 同构, 保证包内 ../../types/ 与 shim 的 ../<pkg>/types/ 相对引用
原样可解析, 从而无需在源码与部署产物之间改写路径):
  <out>/jz_engine/jz_types/types/*.d.ts         <- <wrap>/types
  <out>/jz_engine/jz_types/<pkg>/types/*.d.ts   <- <wrap>/<pkg>/types
  <out>/jz_engine/jz_js/<pkg>/dist/*            <- <wrap>/<pkg>/dist

类型 ambient 声明的 TS7 兼容:
  源码里是标准写法 declare module 'fs' { export * from './fs'; }(位于 <pkg>/types/ambient.d.ts),
  但随工程发布的 TS 7.0.2(tsgo) 在 declare module 内不支持相对路径的通配再导出
  (会静默不导出任何成员), 故部署副本里由 patch_ambient_shims() 追加一行显式具名再导出
  (源码不动, TS5/TS7 都可用)。

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
import re
import shutil
import sys

# js_wrapper 根下的共享类型目录(不是包)
SHARED_TYPES_DIR = "types"
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


def copy_tree_clean(src, dst, label, bundle_root, hint=""):
    """清理目标子目录(仅限脚本自有输出)后拷贝 src -> dst, 避免旧文件残留。"""
    if not os.path.isdir(src):
        print(f"[package.py] WARN skip {label}: source not found: {src}"
              + (f" ({hint})" if hint else ""))
        return False
    _safe_clean(dst, bundle_root)
    shutil.copytree(src, dst)
    print(f"[package.py] {label}: {src} -> {dst}")
    return True


def find_packages(wrap):
    """js_wrapper 下带 types/ 或 dist/ 的一级子目录即运行时包(共享目录 types/ 除外)。"""
    pkgs = []
    for name in sorted(os.listdir(wrap)):
        if name == SHARED_TYPES_DIR or name.startswith("."):
            continue
        d = os.path.join(wrap, name)
        if not os.path.isdir(d):
            continue
        if os.path.isdir(os.path.join(d, SHARED_TYPES_DIR)) or os.path.isdir(os.path.join(d, "dist")):
            pkgs.append(name)
    return pkgs


# ---------------------------------------------------------------------------
# ambient 模块声明的 TS7(tsgo) 兼容补丁
# ---------------------------------------------------------------------------
# 各包的 ambient 声明文件(<pkg>/types/ambient.d.ts)用标准写法:
#   declare module 'fs' { export * from './fs'; }
# 但实测随工程发布的 TS 7.0.2(tsgo) 在 declare module 内部**不支持相对路径的通配再导出**
# (export * from '<相对路径>' 会静默地不导出任何成员 => 用户侧 import type { Stats } from 'fs'
#  报 TS2305 "has no exported member"), 而这两种写法可用:
#   - export { A, B } from '<相对路径>'      (显式具名再导出)
#   - export * from '<另一个 ambient 模块名>' (非相对)
# 所以部署副本里给每个这样的 ambient 块追加一行"显式具名再导出"(源码保持标准写法不动):
#   TS7 走具名那行; TS5 两种写法都认, 显式导出优先, 不会重复报错。
AMBIENT_RE = re.compile(r"declare\s+module\s+'([^']+)'\s*\{([^{}]*)\}", re.S)
STAR_FROM_RE = re.compile(r"export\s+\*\s+from\s+'([^']+)'")
NAMED_FROM_RE = re.compile(r"^export\s+(?:type\s+)?\{([^}]*)\}", re.M)
DECL_RE = re.compile(r"^export\s+(?:declare\s+)?(?:abstract\s+)?"
                     r"(?:interface|type|class|const|let|var|function|enum)\s+([A-Za-z_$][\w$]*)", re.M)


def _resolve_module(base_dir, spec):
    """把 (无扩展名的)相对模块说明符解析为磁盘上的 .d.ts / .ts 文件。"""
    base = os.path.normpath(os.path.join(base_dir, spec))
    for cand in (base + ".d.ts", base + ".ts", os.path.join(base, "index.d.ts")):
        if os.path.isfile(cand):
            return cand
    return None


def _collect_exported_names(path, seen=None):
    """递归收集某类型文件导出的成员名(含 export * / export {..} from 链)。"""
    if seen is None:
        seen = set()
    if path in seen:
        return set()
    seen.add(path)
    names = set()
    try:
        with open(path, encoding="utf-8") as f:
            text = f.read()
    except OSError:
        return names

    for m in DECL_RE.finditer(text):
        names.add(m.group(1))
    for m in NAMED_FROM_RE.finditer(text):
        for part in m.group(1).split(","):
            part = part.strip()
            if part.startswith("type "):
                part = part[5:].strip()
            if " as " in part:
                part = part.rsplit(" as ", 1)[1].strip()
            if part and part != "default":
                names.add(part)
    for m in STAR_FROM_RE.finditer(text):
        nxt = _resolve_module(os.path.dirname(path), m.group(1))
        if nxt:
            names |= _collect_exported_names(nxt, seen)
    return names


def patch_ambient_shims(jz_types_root):
    """给部署副本里的 ambient 声明补一行显式具名再导出(见上方说明), 使其在 TS7 下可用。

    递归扫描 jz_types 下所有 .d.ts(ambient 声明就近放在各包 types/ 里:
    jz_types/types/**(全局) + jz_types/<pkg>/types/**(模块与 ambient))。
    """
    if not os.path.isdir(jz_types_root):
        return
    for root, _dirs, files in os.walk(jz_types_root):
        for fname in sorted(files):
            if not fname.endswith(".d.ts"):
                continue
            path = os.path.join(root, fname)
            rel = os.path.relpath(path, jz_types_root).replace("\\", "/")
            base_dir = os.path.dirname(path)
            with open(path, encoding="utf-8") as f:
                text = f.read()
            out = text
            for m in list(AMBIENT_RE.finditer(text)):
                star = STAR_FROM_RE.search(m.group(2))
                if not star:
                    continue
                spec = star.group(1)
                target = _resolve_module(base_dir, spec)
                if not target:
                    print(f"[package.py] WARN {rel}: target not found: {spec}")
                    continue
                names = sorted(_collect_exported_names(target))
                if not names:
                    print(f"[package.py] WARN {rel}: no exported member found in {spec}")
                    continue
                body = m.group(2).rstrip() + \
                    f"\n    export {{ {', '.join(names)} }} from '{spec}';\n"
                out = out.replace(m.group(0), f"declare module '{m.group(1)}' {{{body}}}", 1)
                print(f"[package.py] ambient {rel}: '{m.group(1)}' <- {len(names)} named exports")
            if out != text:
                with open(path, "w", encoding="utf-8") as f:
                    f.write(out)


def main():
    ap = argparse.ArgumentParser(description="Package JZ JS engine artifacts.")
    ap.add_argument("--out", required=True,
                    help="output base dir, e.g. <build>/binary/code_agent/bin/<CONFIG>")
    ap.add_argument("--js-wrapper", required=True,
                    help="JZJSEngine/js_wrapper source dir (contains types/, <pkg>/types, <pkg>/dist)")
    args = ap.parse_args()

    out = os.path.abspath(args.out)
    wrap = os.path.abspath(args.js_wrapper)
    if not os.path.isdir(wrap):
        print(f"[package.py] ERROR: --js-wrapper not found: {wrap}", file=sys.stderr)
        return 2

    bundle = os.path.join(out, BUNDLE)
    os.makedirs(bundle, exist_ok=True)   # 只创建, 绝不删除 bundle 本身或 <out>

    # 先整目录清空脚本自有输出(jz_types/jz_js), 避免上次/旧布局的残留文件与本次产物撞名
    # (典型: 旧平铺布局的 jz_types/*.d.ts 残留会与新 jz_types/types/、jz_types/<pkg>/types/
    #  的同名 ambient 声明撞出 TS2300 Duplicate identifier)。
    _safe_clean(os.path.join(bundle, "jz_types"), bundle)
    _safe_clean(os.path.join(bundle, "jz_js"), bundle)

    # 共享声明 -> jz_engine/jz_types/types
    copy_tree_clean(os.path.join(wrap, SHARED_TYPES_DIR),
                    os.path.join(bundle, "jz_types", SHARED_TYPES_DIR),
                    "types:shared", bundle)

    pkgs = find_packages(wrap)
    if not pkgs:
        print(f"[package.py] WARN: no package found under {wrap}")

    # 各包类型 -> jz_engine/jz_types/<pkg>/types
    for pkg in pkgs:
        copy_tree_clean(os.path.join(wrap, pkg, SHARED_TYPES_DIR),
                        os.path.join(bundle, "jz_types", pkg, SHARED_TYPES_DIR),
                        f"types:{pkg}", bundle)

    # TS7(tsgo) 兼容: 给部署副本里各包的 ambient 声明补一行显式具名再导出(源码保持标准写法)
    # 必须在共享声明与各包类型都拷完之后执行(ambient 声明的转发目标在同包 types/ 下)
    patch_ambient_shims(os.path.join(bundle, "jz_types"))

    # 运行时 JS(dist) -> jz_engine/jz_js/<pkg>/dist, 保持兄弟布局供包间相对 import
    any_dist = False
    for pkg in pkgs:
        if copy_tree_clean(os.path.join(wrap, pkg, "dist"),
                           os.path.join(bundle, "jz_js", pkg, "dist"),
                           f"dist:{pkg}", bundle,
                           hint="need `tsc -b js_wrapper` first?"):
            any_dist = True
    if not any_dist:
        print("[package.py] WARN: no dist packages found; runtime JS not packaged")

    print(f"[package.py] done -> {bundle}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
