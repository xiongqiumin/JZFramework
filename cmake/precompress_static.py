#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""为静态文本资源生成同目录 .gz 变体。

JZHttpStaticFiles 已经支持 Accept-Encoding: gzip 时优先发 <file>.gz；
这个脚本只在构建输出目录跑，不碰源码目录。mtime=0 让产物可复现。
"""
from __future__ import annotations

import gzip
import os
import sys

# 已经压缩过的格式不再套 gzip；文本/脚本/图标类收益最大。
EXTS = {
    ".html", ".htm", ".js", ".mjs", ".css", ".json",
    ".svg", ".txt", ".xml", ".map", ".wasm",
}


def compress_tree(root: str) -> int:
    if not os.path.isdir(root):
        return 0
    count = 0
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in (".git", "node_modules")]
        for name in filenames:
            if name.endswith(".gz") or name.endswith(".br"):
                continue
            ext = os.path.splitext(name)[1].lower()
            if ext not in EXTS:
                continue
            src = os.path.join(dirpath, name)
            dst = src + ".gz"
            try:
                with open(src, "rb") as f:
                    data = f.read()
                packed = gzip.compress(data, compresslevel=9, mtime=0)
                if len(packed) >= len(data):
                    continue
                with open(dst, "wb") as f:
                    f.write(packed)
                count += 1
            except OSError as e:
                print("precompress skip %s: %s" % (src, e), file=sys.stderr)
    return count


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print("usage: precompress_static.py <dir> [dir ...]", file=sys.stderr)
        return 2
    total = 0
    for root in argv[1:]:
        total += compress_tree(root)
    return 0 if total >= 0 else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
