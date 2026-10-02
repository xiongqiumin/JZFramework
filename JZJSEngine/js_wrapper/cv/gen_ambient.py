#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""重新生成 cv/types/ambient.d.ts（'cv' 模块的显式具名再导出表）。

为什么要显式列名:
    TS7 + moduleResolution:bundler 下, ambient 里 `export * from './x'` 不会转发**具名导出**
    (只转发 default)，所以必须一个个列出来 —— 漏一个, 用户工程 `import { X } from 'cv'` 就报 TS2305。
    机器列名比手抄靠谱, 所以由本脚本从 types/index.d.ts 的**实际声明**里抽。

为什么只从 './index' 再导出:
    部署到 jz_types 的只有 <pkg>/types/**, 实现(./core、./consts)不在那儿 —— 所以公开面必须
    集中在 types/index.d.ts（由 gen_types.py 从 dist/*.d.ts 合并出来）。

用法（改了 core.ts / index.ts 之后）:
    <tsc> -b JZJSEngine/js_wrapper/cv/tsconfig.json
    python JZJSEngine/js_wrapper/cv/gen_types.py       # 合并值声明
    <tsc> -b JZJSEngine/js_wrapper/cv/tsconfig.json
    python JZJSEngine/js_wrapper/cv/gen_ambient.py     # 生成再导出表
"""
import os
import re
import sys

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

CV = os.path.dirname(os.path.abspath(__file__))
TYPES = os.path.join(CV, "types", "index.d.ts")
AMBIENT = os.path.join(CV, "types", "ambient.d.ts")

src = open(TYPES, encoding="utf-8").read()
type_names = sorted(set(re.findall(r"^export (?:type|interface) (\w+)", src, re.M)))
value_names = sorted(set(re.findall(r"^export declare (?:abstract )?(?:class|function|const|let|var) (\w+)", src, re.M)))
if not value_names:
    raise SystemExit("types/index.d.ts 里没找到值声明 —— 先跑 gen_types.py")


def block(names, indent="        ", per=6):
    lines = []
    for i in range(0, len(names), per):
        lines.append(indent + ", ".join(names[i:i + per]) + ",")
    return "\n".join(lines)


out = """// 'cv' —— 对外的裸模块名(= 目录名): 这里只声明名字, 全部转发给同目录的 ./index.d.ts。
//
//   · ./index = cv/types/index.d.ts = **本包对外的单一权威**: 类型(MatInfo/Contour/...)与
//     值(Mat/MatVector/CLAHE/各函数/常量)都在那里。它由 gen_types.py 从 dist/*.d.ts + 手改的
//     类型段合并而来 —— 因为部署到 jz_types 的只有 types/**, 实现 .ts 不在那儿, ambient 若把值
//     指向 '../core' 用户工程就解析不了。
//   · default 也是 './index'(= `import cv from 'cv'` 拿到的 OpenCV.js 形状命名空间)。
//
// ⚠ TS7 + moduleResolution:bundler 下, ambient 内 `export * from './x'` 不转发具名导出(只转发
//   default), 故这里显式列名。⛔ 别手改这两张表: 改 core.ts/index.ts 之后按下面三步重新生成 ——
//       <tsc> -b js_wrapper/cv/tsconfig.json
//       python js_wrapper/cv/gen_types.py
//       <tsc> -b js_wrapper/cv/tsconfig.json && python js_wrapper/cv/gen_ambient.py
declare module 'cv' {
    export type {
%s
    } from './index';
    export {
%s
    } from './index';
    export { default } from './index';
}
""" % (block(type_names), block(value_names))

with open(AMBIENT, "w", encoding="utf-8", newline="\n") as f:
    f.write(out)
print("wrote %s: %d 类型, %d 值" % (AMBIENT, len(type_names), len(value_names)))
