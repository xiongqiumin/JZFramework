#!/usr/bin/env python3
"""补齐缺失的 Twemoji PNG (按 fetch_emoji.py 里的清单重新尝试一次)。"""
import os
import sys
import urllib.request

BASE = "https://raw.githubusercontent.com/twitter/twemoji/master/assets/72x72/"
HERE = os.path.dirname(os.path.abspath(__file__))

sys.path.insert(0, HERE)
try:
    from fetch_emoji import CODEPOINTS
except Exception:
    CODEPOINTS = []

# 兜底: 至少保证这几个核心的
for extra in ["2764", "2764-fe0f", "1f44d", "1f600", "1f602", "1f525",
              "1f389", "1f60a", "1f44f", "1f64f", "1f4a9", "1f440"]:
    if extra not in CODEPOINTS:
        CODEPOINTS.append(extra)

ok, fail = 0, []
for name in sorted(set(CODEPOINTS)):
    dst = os.path.join(HERE, name + ".png")
    if os.path.exists(dst) and os.path.getsize(dst) > 0:
        continue
    try:
        data = urllib.request.urlopen(BASE + name + ".png", timeout=20).read()
        if not data:
            raise ValueError("empty")
        with open(dst, "wb") as f:
            f.write(data)
        ok += 1
        print("ok", name)
    except Exception as e:
        fail.append((name, str(e)))

print("fetched=%d failed=%d" % (ok, len(fail)))
for n, e in fail:
    print("  FAIL", n, e)
