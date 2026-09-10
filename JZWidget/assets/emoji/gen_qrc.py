#!/usr/bin/env python3
"""按本目录下的 *.png 生成 emoji.qrc"""
import os
import glob

here = os.path.dirname(os.path.abspath(__file__))
files = sorted(os.path.basename(p) for p in glob.glob(os.path.join(here, "*.png")))

lines = ['<!DOCTYPE RCC>', '<RCC>', '    <qresource prefix="/emoji">']
for f in files:
    lines.append('        <file alias="%s">%s</file>' % (f, f))
lines += ['    </qresource>', '</RCC>', '']

with open(os.path.join(here, "emoji.qrc"), "w", encoding="utf-8") as fp:
    fp.write("\n".join(lines))

total = sum(os.path.getsize(os.path.join(here, f)) for f in files)
print("files=%d total=%d bytes (%.1f KB)" % (len(files), total, total / 1024.0))
