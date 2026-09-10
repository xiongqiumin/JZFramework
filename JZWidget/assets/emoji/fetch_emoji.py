#!/usr/bin/env python3
"""下载常用 Twemoji (72x72 PNG) 到本目录, 并生成 emoji.qrc。

Twemoji 的文件名就是**码点串** (小写十六进制, 多码点用 '-' 连接),
与运行时 "emoji 文本 -> 文件名" 的规则完全一致, 因此不需要映射表。

用法:  python fetch_emoji.py
"""
import os
import sys
import urllib.request

BASE = "https://raw.githubusercontent.com/twitter/twemoji/master/assets/72x72/"

# 常用 emoji 的码点串 (Twemoji 文件名, 不含 .png)
CODEPOINTS = [
    # ---- 表情 / 情绪 ----
    "1f600", "1f603", "1f604", "1f601", "1f606", "1f605", "1f602", "1f923",
    "1f60a", "1f642", "1f643", "1f609", "1f60c", "1f60d", "1f929", "1f618",
    "1f617", "1f61a", "1f619", "1f60b", "1f61b", "1f61c", "1f92a", "1f911",
    "1f914", "1f928", "1f610", "1f611", "1f636", "1f60f", "1f612", "1f644",
    "1f62c", "1f62e", "1f62f", "1f632", "1f633", "1f97a", "1f626", "1f627",
    "1f628", "1f630", "1f625", "1f622", "1f62d", "1f631", "1f616", "1f623",
    "1f61e", "1f613", "1f629", "1f62b", "1f971", "1f624", "1f621", "1f620",
    "1f92c", "1f608", "1f47f", "1f480", "1f4a9", "1f921", "1f479", "1f47a",
    "1f47b", "1f47d", "1f916", "1f63a", "1f638", "1f639", "1f63b", "1f63c",
    # ---- 手势 / 身体 ----
    "1f44d", "1f44e", "1f44c", "1f44f", "1f64c", "1f64f", "1f44a", "270a",
    "270c", "1f44b", "1f91a", "1f590", "270b", "1f596", "1f44c", "1f919",
    "1f448", "1f449", "1f446", "1f447", "261d", "1f44a", "1f91e", "1f91f",
    "270d", "1f485", "1f440", "1f441", "1f445", "1f444", "1f443", "1f442",
    "1f4aa", "1f9b5", "1f9b6", "1f9b4", "1f440", "1f9e0", "1f9b7", "1f9b8",
    # ---- 爱心 / 符号 ----
    "2764-fe0f", "1f9e1", "1f49b", "1f49a", "1f499", "1f49c", "1f5a4",
    "1f494", "2764", "1f495", "1f49e", "1f493", "1f497", "1f496", "1f498",
    "1f49d", "1f49f", "262e", "271d", "262a", "1f549", "2638", "2721",
    "2b50", "1f31f", "2728", "26a1", "1f525", "1f4a5", "1f4ab", "1f4a2",
    "1f4a6", "1f4a8", "1f4a4", "1f4a2", "1f4af", "1f4a2", "1f389", "1f38a",
    "2705", "274c", "2b55", "2757", "2753", "2755", "203c", "2049",
    "1f6ab", "1f6ad", "1f51e", "1f4af", "1f51d", "1f195", "1f198",
    # ---- 常用物件 / 生活 ----
    "1f4f1", "1f4bb", "2328", "1f5a5", "1f5a8", "1f4be", "1f4bf", "1f4c0",
    "1f4f7", "1f4f8", "1f4f9", "1f3a5", "1f4fa", "1f4fb", "1f4de", "1f4df",
    "1f4e0", "1f4e1", "1f50b", "1f50c", "1f4a1", "1f526", "1f56f", "1f4b0",
    "1f4b5", "1f4b3", "1f48e", "2696", "1f527", "1f528", "1f529", "1f52c",
    "1f52d", "1f4da", "1f4d3", "1f4d4", "1f4d5", "1f4d6", "1f4d7", "1f4d8",
    "1f4d9", "1f4d2", "1f4c5", "1f4c6", "1f4c7", "1f4c8", "1f4c9", "1f4ca",
    "1f5d1", "1f512", "1f513", "1f511", "1f510", "1f5dd", "1f527", "1f517",
    # ---- 交通 / 天气 / 自然 ----
    "1f697", "1f695", "1f699", "1f68c", "1f68e", "1f693", "1f691", "1f692",
    "1f690", "1f69a", "1f69b", "1f69c", "1f6b2", "1f6f5", "1f3cd", "1f3ce",
    "2708", "1f680", "1f681", "1f682", "1f684", "1f685", "1f686", "1f687",
    "1f688", "1f689", "1f68a", "1f6a2", "26f5", "1f6a4", "1f6f3", "1f6e0",
    "2600", "1f31e", "1f31d", "1f31a", "1f319", "2601", "26c5", "26c8",
    "1f324", "1f325", "1f326", "1f327", "1f328", "1f329", "1f32a", "1f32b",
    "1f32c", "1f308", "1f30a", "1f525", "2744", "26a1", "1f30d", "1f30e",
    "1f30f", "1f310", "1f5fa", "1f30b", "1f30c", "1f3d4", "26f0",
    # ---- 动物 ----
    "1f436", "1f431", "1f42d", "1f439", "1f430", "1f98a", "1f43b", "1f43c",
    "1f428", "1f42f", "1f981", "1f42e", "1f437", "1f438", "1f435", "1f414",
    "1f427", "1f426", "1f424", "1f423", "1f986", "1f985", "1f989", "1f987",
    "1f43a", "1f417", "1f434", "1f984", "1f41d", "1f41b", "1f98b", "1f40c",
    "1f41e", "1f41c", "1f99f", "1f997", "1f577", "1f578", "1f982", "1f422",
    # ---- 食物 ----
    "1f34e", "1f34a", "1f34b", "1f34c", "1f349", "1f347", "1f353", "1f348",
    "1f352", "1f351", "1f34d", "1f965", "1f95d", "1f345", "1f346", "1f955",
    "1f33d", "1f336", "1f952", "1f96c", "1f966", "1f9c4", "1f9c5", "1f344",
    "1f330", "1f35e", "1f950", "1f956", "1f968", "1f9c0", "1f95a", "1f373",
    "1f95e", "1f9c7", "1f953", "1f969", "1f357", "1f356", "1f32d", "1f354",
    "1f35f", "1f355", "1f96a", "1f32e", "1f32f", "1f959", "1f96f", "1f35c",
    "1f35d", "1f360", "1f363", "1f371", "1f358", "1f359", "1f35a", "1f35b",
    "1f372", "1f35e", "1f369", "1f36a", "1f382", "1f370", "1f9c1", "1f36b",
    "1f37f", "1f9cb", "1f375", "1f376", "1f37a", "1f37b", "1f377", "1f379",
    # ---- 手势补充 / 常用标记 ----
    "1f4a4", "1f4ac", "1f4ad", "1f5ef", "1f4a3", "1f573", "1f441-fe0f-200d-1f5e8",
    "1f4a4", "1f44b", "1f91d", "1f64c", "1f450", "1f64f", "1f91c", "1f91b",
]

# 常用的人脸/手势等方向变体不多, 上面已覆盖。


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    names = sorted(set(CODEPOINTS))

    ok, fail = 0, []
    for name in names:
        dst = os.path.join(here, name + ".png")
        if os.path.exists(dst) and os.path.getsize(dst) > 0:
            ok += 1
            continue
        url = BASE + name + ".png"
        try:
            data = urllib.request.urlopen(url, timeout=20).read()
            if not data:
                raise ValueError("empty body")
            with open(dst, "wb") as f:
                f.write(data)
            ok += 1
        except Exception as e:
            fail.append((name, str(e)))

    print(f"downloaded/kept: {ok}  failed: {len(fail)}")
    for n, e in fail[:20]:
        print(f"  FAIL {n}: {e}")

    # ---- 生成 emoji.qrc ----
    files = sorted(f for f in os.listdir(here)
                   if f.endswith(".png"))
    lines = ['<!DOCTYPE RCC>', '<RCC>', '    <qresource prefix="/emoji">']
    for f in files:
        lines.append(f'        <file alias="{f}">{f}</file>')
    lines.append('    </qresource>')
    lines.append('</RCC>')
    lines.append('')

    qrc = os.path.join(here, "emoji.qrc")
    with open(qrc, "w", encoding="utf-8") as fp:
        fp.write("\n".join(lines))
    print(f"wrote {qrc} with {len(files)} files")


if __name__ == "__main__":
    main()
