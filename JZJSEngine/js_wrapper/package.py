#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
打包 JZ JS 引擎产物, 供部署(拷到 exe 同配置目录 bin/<CONFIG>/)。
产出 = **一棵与源码同构的 js_wrapper/ 树** —— 源码只有一个 js_wrapper 目录, 部署也只有一个:
  <out>/js_wrapper/types/*.d.ts        : 共享/全局声明(console/fetch/URL/WebSocket/Buffer...)
  <out>/js_wrapper/<pkg>/types/*.d.ts  : 该包的模块类型 + ambient(declare module '<裸名>')
  <out>/js_wrapper/<pkg>/dist/*        : 运行时 JS(tsc 编译产物, 含 tsc 顺带 emit 的 .d.ts)

1:1 布局的价值: 与源码**逐目录同构** ⇒ 手写定义里的 `/// <reference path="../../types/common.d.ts" />`
与 ambient 的 `from './index'` 等相对引用**原样可解析**, 打包不需要改写任何路径; 用户/模型看部署
目录就等于看源码目录(只是不含 index.ts / package.json / tsconfig.json 这些源码专属文件)。

类型来源(单一来源 = js_wrapper 自身, 已不再有 JZJSEngine/types):
  <wrap>/types/          : 共享声明 —— 全局类/console/fetch/EventEmitter 等 + 各模块的
                           ambient 转发 shim(declare module 'fs' ...), 供 import type 使用
  <wrap>/<pkg>/types/    : 各包类型(模块形式, 与包内实现同源)

类型 ambient 声明的 TS7 兼容:
  源码里是标准写法 declare module 'fs' { export * from './fs'; }(位于 <pkg>/types/ambient.d.ts),
  但随工程发布的 TS 7.0.2(tsgo) 在 declare module 内不支持相对路径的通配再导出
  (会静默不导出任何成员), 故部署副本里由 patch_ambient_shims() 追加一行显式具名再导出
  (源码不动, TS5/TS7 都可用)。

删除策略(重要):
  本脚本的输出就是 <out>/js_wrapper 这一棵(完全由它生成) ⇒ 每次先**整棵删掉**再重铺,
  这样源码里删掉的包/文件不会在部署里留残留(旧版按 jz_types/jz_js 两个子目录清, 现在合成
  一棵, 整棵删才干净)。删前做路径守卫: 只允许删"名字 = js_wrapper"的目录本身, 绝不碰它的
  父目录(<out>)与 <out> 下的其它任何东西。

依赖: dist 由 `tsc -b js_wrapper` 生成(非 CMake target)。某包 dist 缺失时跳过并告警。
"""
import argparse
import os
import re
import shutil
import sys

# js_wrapper 根下的共享类型目录(不是包)
SHARED_TYPES_DIR = "types"
# 部署目录名 = **源码目录名**(1:1): 源码一个 js_wrapper, 部署也只有一个。
BUNDLE = "js_wrapper"


def _clean_bundle(bundle):
    """整棵删掉上次的部署目录(后面重铺)。

    守卫: 只允许删"名字 = BUNDLE"的目录本身 —— 绝不碰它的父目录(<out>)与 <out> 下的其它东西。
    """
    ap = os.path.abspath(bundle)
    if os.path.basename(ap) != BUNDLE:
        raise RuntimeError(f"[package.py] REFUSE to delete non-bundle dir: {ap}")
    if os.path.isdir(ap):
        shutil.rmtree(ap)


def copy_tree(src, dst, label, hint=""):
    """拷贝 src -> dst(src 不存在时告警跳过, 由调用方决定要不要当真)。"""
    if not os.path.isdir(src):
        print(f"[package.py] WARN skip {label}: source not found: {src}"
              + (f" ({hint})" if hint else ""))
        return False
    shutil.copytree(src, dst)
    print(f"[package.py] {label}: {src} -> {dst}")
    return True


def find_packages(wrap):
    """js_wrapper 下带 types/ 或 dist/ 的一级子目录即运行时包(共享目录 types/ 除外)。

    另要求**有源码(index.ts)或有类型(types/)**: 只有 dist/ 的目录是"重命名/拆分后
    残留的陈旧产物"(典型: 历史上的 core/ 包被拆成 fs/path/process 之后只剩 dist/),
    它不是包, 不该打进部署(--js-wrapper 下 dist/ 已 gitignore, 这类目录本就不可复现)。
    """
    pkgs = []
    for name in sorted(os.listdir(wrap)):
        if name == SHARED_TYPES_DIR or name.startswith("."):
            continue
        d = os.path.join(wrap, name)
        if not os.path.isdir(d):
            continue
        has_types = os.path.isdir(os.path.join(d, SHARED_TYPES_DIR))
        has_src = os.path.isfile(os.path.join(d, "index.ts"))
        has_dist = os.path.isdir(os.path.join(d, "dist"))
        if not (has_types or has_src):
            if has_dist:
                print(f"[package.py] SKIP stale dist-only dir (no index.ts/types/): {name}")
            continue
        if has_types or has_dist:
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
                    help="output base dir, e.g. <build>/binary/bin/<CONFIG>")
    ap.add_argument("--js-wrapper", required=True,
                    help="JZJSEngine/js_wrapper source dir (contains types/, <pkg>/types, <pkg>/dist)")
    args = ap.parse_args()

    out = os.path.abspath(args.out)
    wrap = os.path.abspath(args.js_wrapper)
    if not os.path.isdir(wrap):
        print(f"[package.py] ERROR: --js-wrapper not found: {wrap}", file=sys.stderr)
        return 2

    bundle = os.path.join(out, BUNDLE)
    os.makedirs(out, exist_ok=True)
    _clean_bundle(bundle)                 # 整棵重铺(见文件头"删除策略")
    os.makedirs(bundle, exist_ok=True)

    # 共享声明 -> js_wrapper/types
    copy_tree(os.path.join(wrap, SHARED_TYPES_DIR),
              os.path.join(bundle, SHARED_TYPES_DIR),
              "types:shared")

    pkgs = find_packages(wrap)
    if not pkgs:
        print(f"[package.py] WARN: no package found under {wrap}")

    # 各包类型 -> js_wrapper/<pkg>/types(与源码 1:1)
    for pkg in pkgs:
        copy_tree(os.path.join(wrap, pkg, SHARED_TYPES_DIR),
                  os.path.join(bundle, pkg, SHARED_TYPES_DIR),
                  f"types:{pkg}")

    # TS7(tsgo) 兼容: 给部署副本里各包的 ambient 声明补一行显式具名再导出(源码保持标准写法)
    # 必须在共享声明与各包类型都拷完之后执行(ambient 声明的转发目标在同包 types/ 下)
    patch_ambient_shims(bundle)

    # 运行时 JS(dist) -> js_wrapper/<pkg>/dist, 与源码 1:1(兄弟布局供包间相对 import)
    any_dist = False
    for pkg in pkgs:
        if not os.path.isdir(os.path.join(wrap, pkg, "dist")):
            # 没有 dist 的两种情况分开报: 只有类型、没有实现的包(agent)是**正常**的;
            # 有 index.ts 却没 dist = 忘了跑 tsc -b js_wrapper, 那才是要提示的。
            if os.path.isfile(os.path.join(wrap, pkg, "index.ts")):
                print(f"[package.py] WARN skip dist:{pkg}: no dist "
                      f"(need `tsc -b js_wrapper` first?)")
            continue
        if copy_tree(os.path.join(wrap, pkg, "dist"),
                     os.path.join(bundle, pkg, "dist"), f"dist:{pkg}"):
            any_dist = True
    if not any_dist:
        print("[package.py] WARN: no dist packages found; runtime JS not packaged")

    print(f"[package.py] done -> {bundle}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
