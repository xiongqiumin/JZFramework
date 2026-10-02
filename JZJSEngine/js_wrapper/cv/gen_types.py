#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把 cv 的**公开面**合并进 types/index.d.ts（类型 + 值 + 常量）。

为什么要合并（而不是像包内那样直接从 ./core、./consts 再导出）:
    部署到 jz_types 的**只有** <pkg>/types/**（见 js_wrapper/package.py），实现 .ts 不在那儿。
    所以用户工程能看到的只有 .d.ts —— 本包的 ambient 若把值指向 '../core'，用户工程就会
    "找不到模块"。仓库里的现成做法（computeruse / sqlite / buffer ...）就是：
    **types/index.d.ts 同时声明类型与值**, ambient 只从 './index' 再导出。

输入:
    cv/dist/{units,core,index,consts}.d.ts      <- 由 `tsc -b js_wrapper/cv` 产出（声明文件）
    cv/types/index.d.ts                         <- 本文件里"手改段"(类型定义, 到 MARK 为止)

输出:
    cv/types/index.d.ts = [手改段] + MARK + [生成段(值/常量) + default]

用法（改了 core.ts / index.ts / consts.ts / units.ts 之后）:
    <tsc> -b JZJSEngine/js_wrapper/cv/tsconfig.json     # 先出 dist/*.d.ts
    python JZJSEngine/js_wrapper/cv/gen_types.py
    <tsc> -b JZJSEngine/js_wrapper/cv/tsconfig.json     # 再过一遍确认
"""
import os
import re
import sys

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

CV = os.path.dirname(os.path.abspath(__file__))
DIST = os.path.join(CV, "dist")
TYPES = os.path.join(CV, "types", "index.d.ts")

MARK = "// ==================== 以下为**生成段**（勿手改, 见 gen_types.py） ===================="

DECL_RE = re.compile(r"^export declare (?:abstract )?(?:class|function|const|let|var) (\w+)", re.M)
IMPORT_RE = re.compile(r"^import\b.*?;\s*$", re.M | re.S)
REEXPORT_RE = re.compile(r"^export \* from .*?;\s*$", re.M)
DEFAULT_RE = re.compile(r"^declare const (\w+): \{[\s\S]*?\n\};\nexport default \1;\s*$", re.M)


def declared_names(text):
    return set(DECL_RE.findall(text))


def strip_module_lines(text):
    """去掉 import / export * / 具名再导出 —— 合并后这些名字都在同一个文件里了。"""
    text = IMPORT_RE.sub("", text)
    text = REEXPORT_RE.sub("", text)
    text = re.sub(r"^export (?:type )?\{[^}]*\} from .*?;\s*$", "", text, flags=re.M)
    text = re.sub(r"\n{3,}", "\n\n", text)
    return text.strip()


def read(name):
    with open(os.path.join(DIST, name), encoding="utf-8") as f:
        return f.read()


def drop_decls(text, names):
    """去掉 text 里名字在 names 里的声明（整块: 多行签名 + 类体), 连带它上面的 JSDoc 注释。
    做法是**括号配平**逐行扫, 而不是按行丢 —— 多行签名的尾巴(`}): Mat;`)也得吃掉。"""
    out = []
    lines = text.split("\n")
    i = 0
    while i < len(lines):
        line = lines[i]
        m = re.match(r"^export declare (?:abstract )?(?:class|function) (\w+)", line)
        if m and m.group(1) in names:
            depth = 0
            while i < len(lines):
                cur = lines[i]
                depth += cur.count("{") + cur.count("(") - cur.count("}") - cur.count(")")
                i += 1
                if depth <= 0 and (cur.rstrip().endswith(";") or cur.rstrip().endswith("}")):
                    break
            # 连注释一起丢掉（不然留一堆指向已删声明的 /** ... */）
            while out and out[-1].strip().startswith(("*/", "*", "/**")):
                out.pop()
            continue
        out.append(line)
        i += 1
    return "\n".join(out)


def main():
    units = strip_module_lines(read("units.d.ts"))
    core = strip_module_lines(read("core.d.ts"))
    index = strip_module_lines(read("index.d.ts"))
    consts = strip_module_lines(read("consts.d.ts"))

    # 手改段（类型定义）：文件里 MARK 之前的内容；第一次跑时 = 整个旧文件
    with open(TYPES, encoding="utf-8") as f:
        old = f.read()
    head = old.split(MARK)[0].rstrip()

    # index.d.ts(入口层) 与 core.d.ts 有同名函数（入口层是 OpenCV.js 形状的重载版）——
    # 只留入口层那份, 免得同一个名字出现两套签名。
    layer_names = declared_names(index)
    core_lines = []
    drop_block = False
    for line in core.split("\n"):
        if drop_block:
            if line == "}":
                drop_block = False
            continue
        core_lines.append(line)
    core = drop_decls("\n".join(core_lines), layer_names)

    # core 的 default（我们自己的"顺手"形状）不对外；入口层的 default 才是 cv 命名空间
    core = DEFAULT_RE.sub("", core).rstrip()

    body = "\n\n".join([units, core, consts, index])
    body = re.sub(r"\n{3,}", "\n\n", body).strip()

    out = head + "\n\n" + MARK + "\n" + body + "\n"
    with open(TYPES, "w", encoding="utf-8", newline="\n") as f:
        f.write(out)

    print("wrote %s  (%d 行; 值声明 %d 个, 常量 %d 个)"
          % (TYPES, out.count("\n") + 1, len(declared_names(out)), len(re.findall(r"^export declare const", consts, re.M))))


if __name__ == "__main__":
    main()
