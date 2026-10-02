#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gba 任务 —— 尺子（断言 / 判分 / 一页摘要）

## 先说清楚：这把尺子**不判"通关"**
"通关"没有自动真值（这一版刻意不让尺子读模拟器内存/存档；画面语义也还没有客观口径）。
它判的是**这一局到底跑起来了没有** —— 而且用**两路证据互相印证**：

  · 脚本自报（`session.json` / `play.log` / `frames/`）——"它说它干了什么、看到了什么"；
  · 驱动旁证（`external/` / `metrics.jsonl`）——"从外面看，那块屏当时到底是什么样"。

## 硬门（名字与 baseline.json 的 gates 必须一致）
  G1 container_ok     容器起来了、游戏窗口在了（拿到客户区几何）
  G2 window_native    客户区 = 240x160（GBA 原生 1:1）
  G3 script_ran       脚本自己写了 session.json（= 它跑完收工了，没卡死）
  G4 script_acted     脚本自报**按过键**（它在操作，不是干看着）
  G5 external_alive   外面抓的帧够数、尺寸都对（证据链完整：全程有人盯着这块屏）
  G6 screen_alive     外部帧里至少一帧不是单色块（那是真画面，不是白屏/黑屏卡住）
  G7 screen_changed   外部帧序列里至少出现一次显著变化（画面在动 = 游戏在跑/在响应）

全过 ⇒ **PASS**（退出码 0）—— **故意不打 `[loop-stop]`**：跑起来 ≠ 通关，
收工由人看归档里的画面决定（这一版就这样，见 prompt.md 的"判定"）。

## 用法（本目录下）
  python verify.py                        # 判 runs/current（run.py 跑完自动调它）
  python verify.py --dir runs/round_003   # 判某一轮归档
  python verify.py --check-only           # 只打印结论，不写产物（外部复验用）
  python verify.py --self-test            # 尺子自检：伪造一好一坏两局，好局必须全过、坏局必须报对门
"""

import argparse
import json
import os
import sys

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

HERE = os.path.dirname(os.path.abspath(__file__))
CUR = os.path.join(HERE, "runs", "current")
sys.path.insert(0, HERE)
import env as E  # noqa: E402

# 硬门（与 baseline.json 的 gates 一致；改口径要两边一起改）
GATES = ["container_ok", "window_native", "script_ran", "script_acted",
         "external_alive", "screen_alive", "screen_changed"]

NATIVE_W, NATIVE_H = 240, 160
DRIFT_THRESHOLD = 0.01          # 外部帧之间差多少算"画面变了"（1%）


def _load_json(path):
    return E.read_json(path, None)


def _metrics(path):
    out = []
    try:
        with open(path, encoding="utf-8") as f:
            for line in f:
                if line.strip():
                    out.append(json.loads(line))
    except Exception:
        pass
    return out


def judge(root):
    """对一局（runs/current 或 runs/round_NNN）给出逐门结论 + 给人/模型看的摘要。"""
    drive = _load_json(os.path.join(root, "drive.json")) or {}
    session = _load_json(os.path.join(root, "session.json")) or {}
    metrics = _metrics(os.path.join(root, "metrics.jsonl"))
    ext_dir = os.path.join(root, "external")
    ext = sorted(f for f in os.listdir(ext_dir) if f.endswith(".png")) \
        if os.path.isdir(ext_dir) else []

    res = {"gates": [], "metrics": {}, "detail": {}, "root": root}
    if not drive and not session:
        for name in GATES:
            res["gates"].append((name, False, "这一局没有产物（run.py 没跑起来？）"))
        return res

    client = drive.get("clientSize") or []
    cw = int(client[0]) if len(client) >= 2 else 0
    ch = int(client[1]) if len(client) >= 2 else 0

    # G1 容器/窗口（容器里应当就是两个进程：gba=游戏机 + script=脚本）
    progs = drive.get("programs") or []
    ok = cw > 0 and ch > 0
    res["metrics"]["programs"] = progs
    res["gates"].append(("container_ok", ok,
                         "容器进程=%s；游戏窗口 title=%s 客户区=%s"
                         % (progs or "（没记到）", drive.get("window"), client or "（没拿到）")))

    # G2 原生尺寸
    res["gates"].append(("window_native", (cw, ch) == (NATIVE_W, NATIVE_H),
                         "客户区 = %sx%s（期望 %dx%d）" % (cw, ch, NATIVE_W, NATIVE_H)))

    # G3 脚本收工（session.json 是 play.ts 自己写的）
    ok = bool(session) and "reason" in session
    res["gates"].append(("script_ran", ok,
                         "脚本自报: %s" % (("%s，%s 拍，%s 张帧，用时 %sms"
                                        % (session.get("reason"), session.get("steps"),
                                           len(session.get("frames") or []), session.get("elapsedMs")))
                                       if session else "没有 session.json（卡住了/崩了？）")))

    # G4 脚本真的在操作
    keys = session.get("keys") or []
    res["metrics"]["scriptKeys"] = len(keys)
    res["metrics"]["scriptKeySteps"] = len(session.get("frames") or [])
    res["gates"].append(("script_acted", len(keys) > 0,
                         "脚本按过 %d 次键（%s）" % (len(keys), "/".join(sorted(set(keys))[:12]) or "无")))

    # G5 外部证据链
    expect = max(5, int((drive.get("steps") or 0) * 0.5))
    sizes_bad = [m for m in metrics if (m.get("w"), m.get("h")) != (NATIVE_W, NATIVE_H)]
    ok = len(ext) >= min(expect, 5) and not sizes_bad
    res["metrics"]["externalFrames"] = len(ext)
    res["metrics"]["externalExpected"] = expect
    res["gates"].append(("external_alive", ok,
                         "外面抓到 %d 张（期望≥%d，尺寸不对 %d 张）"
                         % (len(ext), min(expect, 5), len(sizes_bad))))

    # G6 画面真有内容（外部帧）
    best = None
    for m in metrics:
        # "内容最丰富"按 (颜色数, 非黑比例) 取最大 —— 只看颜色数会在"整块单色"之间乱挑
        if best is None or ((m.get("colors") or 0), (m.get("nonblack") or 0.0)) > \
                ((best.get("colors") or 0), (best.get("nonblack") or 0.0)):
            best = m
    if best is None and ext:                       # 归档里 metrics 可能被裁掉：直接看图
        for f in ext[::max(1, len(ext) // 8)]:
            img = E.read_png(os.path.join(ext_dir, f))
            m = {"file": os.path.join("external", f), "colors": E.unique_colors(img),
                 "nonblack": E.nonblack_ratio(img), "drift": 0.0}
            if best is None or m["colors"] > best["colors"]:
                best = m
    ok = bool(best) and ((best.get("colors") or 0) >= 4 or (best.get("nonblack") or 0) > 0.05)
    res["metrics"]["bestFrame"] = ({k: best.get(k) for k in ("file", "colors", "nonblack", "drift")}
                                   if best else None)
    res["gates"].append(("screen_alive", ok, "内容最丰富的一帧: %s" % (res["metrics"]["bestFrame"],)))

    # G7 画面在动（外部帧序列里有显著变化）
    drifts = [m.get("drift") or 0.0 for m in metrics]
    changed = [d for d in drifts if d >= DRIFT_THRESHOLD]
    if not metrics and len(ext) >= 2:              # 同上：没有 metrics 就用图现算
        prev = None
        for f in ext:
            img = E.read_png(os.path.join(ext_dir, f))
            if prev is not None:
                d = E.diff_ratio(prev, img)
                if d >= DRIFT_THRESHOLD:
                    changed.append(d)
            prev = img
    res["metrics"]["maxDrift"] = round(max(drifts), 4) if drifts else None
    res["metrics"]["changedFrames"] = len(changed)
    res["gates"].append(("screen_changed", len(changed) > 0,
                         "画面显著变化 %d 次（最大差异 %s）"
                         % (len(changed), ("%.2f%%" % (max(drifts) * 100)) if drifts else "（没测到）")))

    res["metrics"].update({
        "elapsedSec": drive.get("elapsedSec"),
        "driveErrors": drive.get("errors") or [],
        "scriptErrors": session.get("errors") or [],
        "scriptReason": session.get("reason"),
    })
    res["notes"] = (session.get("keys") or [])[:40]
    return res


def summary_lines(res):
    m = res.get("metrics", {})
    out = ["本局: 容器里两个进程 %s；外面抓帧 %s 张（用时 %ss）| 脚本自报按键 %s 次、%s 拍、停因=%s"
           % (m.get("programs") or "?" , m.get("externalFrames"), m.get("elapsedSec"),
              m.get("scriptKeys"), m.get("scriptKeySteps"), m.get("scriptReason"))]
    if m.get("bestFrame"):
        b = m["bestFrame"]
        out.append("最像画面的一帧: %s（颜色数=%s，非黑=%.3f）—— 人看这一张"
                   % (b.get("file"), b.get("colors"), b.get("nonblack") or 0.0))
    out.append("画面显著变化 %s 次（最大差异 %s）"
               % (m.get("changedFrames"), ("%.2f%%" % (m.get("maxDrift") * 100))
                  if m.get("maxDrift") is not None else "（没测到）"))
    for e in (m.get("driveErrors") or []):
        out.append("驱动侧: %s" % e)
    for e in (m.get("scriptErrors") or []):
        out.append("脚本侧: %s" % e)
    return out


def write_report(root, res, session):
    """一页摘要 + 关键帧（markdown 直接引用图片 ⇒ 大模型/人一眼看到"界面"）。"""
    ext_dir = os.path.join(root, "external")
    ext = sorted(f for f in os.listdir(ext_dir) if f.endswith(".png")) \
        if os.path.isdir(ext_dir) else []
    keep = ext[::max(1, len(ext) // 6)] + ([ext[-1]] if ext and ext[-1] not in ext[::max(1, len(ext) // 6)] else [])
    lines = ["# 一局记录（play.ts 在容器里跑）", "",
             "- 结果: %s" % ("PASS" if all(ok for _, ok, _ in res["gates"]) else "FAIL"),
             "- 容器里的进程: %s（一个是游戏 `gba`，一个是脚本 `script`）"
             % (res["metrics"].get("programs") or "（没记到）"),
             "- 参数: %s" % json.dumps(session.get("cfg") or {}, ensure_ascii=False),
             "- 脚本自报: reason=%s steps=%s 按键=%s 次 帧=%s 张"
             % (session.get("reason"), session.get("steps"), len(session.get("keys") or []),
                len(session.get("frames") or [])), ""]
    lines += ["## 硬门", ""] + ["- %s %s：%s" % ("[ok]" if ok else "[FAIL]", n, info)
                                for n, ok, info in res["gates"]]
    lines += ["", "## 外面看到的画面（外部抓帧，每若干张选一张）", ""]
    for f in keep:
        lines.append("![%s](external/%s)" % (f, f))
    if session.get("keys"):
        lines += ["", "## 脚本按过的键（前 40 次）", "", "`%s`" % " ".join(session["keys"][:40])]
    if session.get("errors") or (res["metrics"].get("driveErrors")):
        lines += ["", "## 错误", ""]
        lines += ["- 脚本: %s" % e for e in (session.get("errors") or [])]
        lines += ["- 驱动: %s" % e for e in (res["metrics"].get("driveErrors") or [])]
    try:
        with open(os.path.join(root, "report.md"), "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
    except Exception as e:
        print("WARN: 写 report.md 失败: %s" % e)


def run(root, check_only):
    res = judge(root)
    for name, ok, info in res["gates"]:
        print("%s %-16s %s" % ("[ ok ]" if ok else "[FAIL]", name, info))
    failed = [n for n, ok, _ in res["gates"] if not ok]
    for line in summary_lines(res):
        print(line)
    if failed:
        print("--- 没过的是这些门，下一轮就照它们改（别同时改五处）---")
        for n in failed:
            print("  - %s" % n)
    print("PASS" if not failed else "FAIL")
    if not failed:
        print("(这一局跑起来了；但『通关』没有自动判据 —— 不打 [loop-stop]，请看归档的画面)")

    if not check_only:
        try:
            os.makedirs(CUR, exist_ok=True)
            with open(os.path.join(root, "result.json"), "w", encoding="utf-8") as f:
                json.dump({"pass": not failed, "gates": res["gates"], "metrics": res["metrics"]},
                          f, ensure_ascii=False, indent=2)
            write_report(root, res, _load_json(os.path.join(root, "session.json")) or {})
        except Exception as e:
            print("WARN: 写产物失败: %s" % e)
    return 0 if not failed else 1


def self_test():
    """尺子自检：伪造一好一坏两局（临时目录），好局必须全过、坏局必须报出对应门。"""
    import shutil
    import tempfile
    import zlib

    def write_png(path, w, h, rows):
        raw = b"".join(b"\x00" + bytes(r) for r in rows)

        def chunk(t, d):
            c = t + d
            return len(d).to_bytes(4, "big") + c + zlib.crc32(c).to_bytes(4, "big")

        png = b"\x89PNG\r\n\x1a\n" + \
            chunk(b"IHDR", w.to_bytes(4, "big") + h.to_bytes(4, "big") + bytes([8, 6, 0, 0, 0])) + \
            chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")
        with open(path, "wb") as f:
            f.write(png)

    def row(rgb, w):
        return [v for _ in range(w) for v in (rgb[0], rgb[1], rgb[2], 255)]

    def make(root, good):
        """好局：6 张外部帧（尺寸对、有一帧内容丰富、序列里有一次显著变化）+ 脚本自报收工；
        坏局：同样 6 张但**全是纯黑**（没内容也没变化），而且脚本**没写出 session.json**（卡死）。"""
        ext = os.path.join(root, "external")
        os.makedirs(ext, exist_ok=True)
        metrics = []
        for i in range(6):
            rows = [row((0, 0, 0), NATIVE_W) for _ in range(NATIVE_H)]
            colors, nonblack, drift = 1, 0.0, 0.0
            if good and i >= 2:                      # 第 3 张起有内容，并且与上一张不同
                rows[10] = row((255, 0, 0), NATIVE_W)
                rows[11] = row((0, 255, 0), NATIVE_W)
                rows[12] = row((0, 0, 255), NATIVE_W)
                colors, nonblack, drift = 4, 3.0 / NATIVE_H, 0.02
            write_png(os.path.join(ext, "e%05d.png" % i), NATIVE_W, NATIVE_H, rows)
            metrics.append({"i": i, "w": NATIVE_W, "h": NATIVE_H, "colors": colors,
                            "nonblack": nonblack, "drift": drift})
        with open(os.path.join(root, "metrics.jsonl"), "w", encoding="utf-8") as f:
            for m in metrics:
                f.write(json.dumps(m) + "\n")
        json.dump({"clientSize": [NATIVE_W, NATIVE_H], "window": "GBA", "steps": 12,
                   "errors": [], "elapsedSec": 3.0},
                  open(os.path.join(root, "drive.json"), "w", encoding="utf-8"))
        if good:
            json.dump({"reason": "steps", "steps": 12, "keys": ["A", "Start"],
                       "frames": ["f00000.png"], "errors": [], "elapsedMs": 3000,
                       "cfg": {"steps": 12}},
                      open(os.path.join(root, "session.json"), "w", encoding="utf-8"))

    tmp = tempfile.mkdtemp(prefix="gba_verify_")
    try:
        good, bad = os.path.join(tmp, "good"), os.path.join(tmp, "bad")
        make(good, True)
        make(bad, False)
        rg, rb = judge(good), judge(bad)
        ok_good = all(ok for _, ok, _ in rg["gates"])
        bad_failed = [n for n, ok, _ in rb["gates"] if not ok]
        ok_bad = set(bad_failed) == {"script_ran", "script_acted", "screen_alive", "screen_changed"}
        # （坏局该失败的正是这四道：脚本没写出 session.json、没按过键；外部帧全黑 ⇒ 没内容、没变化）
        print("self-test 好局 -> %s %s" % ("OK" if ok_good else "FAIL",
                                          [(n, ok) for n, ok, _ in rg["gates"]]))
        print("self-test 坏局 -> %s（该失败的门: %s）" % ("OK" if ok_bad else "FAIL", bad_failed))
        print("SELF-TEST %s" % ("OK" if (ok_good and ok_bad) else "FAIL"))
        return 0 if (ok_good and ok_bad) else 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser(description="gba 任务断言（跑没跑起来 / 证据齐不齐；不判通关）")
    ap.add_argument("--dir", default=CUR, help="判哪一局（默认 runs/current；也可指 runs/round_NNN）")
    ap.add_argument("--check-only", action="store_true", help="只打印结论，不写产物（外部复验用）")
    ap.add_argument("--self-test", action="store_true", help="尺子自检（伪造一好一坏两局）")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    return run(args.dir, args.check_only)


if __name__ == "__main__":
    sys.exit(main())
