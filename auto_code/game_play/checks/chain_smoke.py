#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
game_play 任务 —— 链路自检（"这台游戏机还转得动吗"，不判游戏玩得好不好）

一句话：起容器 → 加载 gba.js（JZEmulatorView）→ 截图 / 按键 / 再截图 → 把能机械
判定的都判掉，把图像证据落盘给人看。

判什么（全部与"玩得好不好"无关）:
  1. 容器起来了（health ok、桌面尺寸对、scale 恒 1）；
  2. 游戏窗口客户区 = 240x160（GBA 原生，1:1 上屏；不放大不超分）；
  3. 客户区截图 PNG 尺寸 == 客户区逻辑尺寸（截图与命中同一坐标系）；
  4. 画面**不是黑屏**（非黑像素比例够高）⇒ ROM 真的跑起来了；
  5. 画面**在动**（连拍数张，帧间差异够大）⇒ 模拟器定时器在跑；
  6. 帧数在涨（状态文件自报）⇒ 同上，另一条独立证据；
  7. 按键**投得进去**（POST 回包 handled=true ⇒ 事件落进了控件树）。
     ⚠ 这一条只证明"信号到了控件的门口"，**不证明游戏响应了它** —— 后者要靠画面语义，
     属于"玩得好不好"，这一版交给人看（见 --out 里的 before/after 截图）。

用法（本目录下）:
  python checks/chain_smoke.py                     # 用默认 ROM（见 task.json 的 rom）
  python checks/chain_smoke.py --rom <other.gba>
  python checks/chain_smoke.py --keep              # 测完不杀容器（手工接着看）
  python checks/chain_smoke.py --keys "Return"     # 只按这一个键（默认试一组）

产物: runs/current/chain/<时间戳>/ 下的 PNG（boot / idle_NN / after_keys）+ 控制台结论。
"""

import argparse
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TASK = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, TASK)

import env as E  # noqa: E402  （同任务的 env.py）

DEFAULT_ROM = os.path.join("E:/work/simulator", "火焰之纹章-圣魔之光石完美修改版.gba")

_fail = []


def check(ok, msg, info=""):
    print("  [%s] %s%s" % (" ok " if ok else "FAIL", msg, ("  -- " + str(info)) if info else ""))
    if not ok:
        _fail.append(msg)
    return ok


def main():
    ap = argparse.ArgumentParser(description="GBA 链路自检")
    ap.add_argument("--cfg", default="Debug")
    ap.add_argument("--rom", default=DEFAULT_ROM)
    ap.add_argument("--dll-dir", default="")
    ap.add_argument("--desktop", default="320x240")
    ap.add_argument("--out", default="")
    ap.add_argument("--keys", default="")
    ap.add_argument("--warmup", type=float, default=6.0,
                    help="加载后先等这么多秒再判画面（GBA 开机 logo 那几帧本来就是纯黑/纯白）")
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    exe = E.container_exe(args.cfg)
    if not os.path.exists(exe):
        print("FAIL: 找不到容器 exe: %s" % exe)
        return 1
    dll_dir = args.dll_dir or E.emulator_dll_dir(args.cfg)
    if not os.path.exists(os.path.join(dll_dir, "mgba_host.dll")):
        print("FAIL: 找不到 mgba_host.dll（在 %s）—— 先构建 JZEmulator 模块" % dll_dir)
        return 1
    game_js = os.path.join(TASK, "build", "game", "gba.js")
    if not os.path.exists(game_js):
        print("FAIL: 找不到 %s\n      先编译: tsc -p auto_code/game_play/tsconfig.json" % game_js)
        return 1
    if not os.path.exists(args.rom):
        print("FAIL: 找不到 ROM: %s" % args.rom)
        return 1

    out = args.out or os.path.join(TASK, "runs", "current", "chain",
                                   time.strftime("%Y%m%d-%H%M%S"))
    os.makedirs(out, exist_ok=True)
    print("== ROM: %s" % args.rom)
    print("== 产物: %s" % os.path.relpath(out, TASK))

    g = E.GameSession(cfg=args.cfg, rom=args.rom, dll_dir=dll_dir, desktop=args.desktop,
                      log_path=os.path.join(out, "container_stdout.txt"),
                      on_log=lambda s: print(s))
    try:
        g.open()

        print("[1/7] 容器")
        h = g.c.health()
        check(h.get("ok") is True, "容器 health ok")
        check(float(h["desktop"]["scale"]) == 1.0, "无头下 scale 恒 1", h["desktop"]["scale"])

        print("[2/7] 游戏窗口")
        check(g.client_size == (E.GBA_W, E.GBA_H),
              "客户区 = 240x160（GBA 原生 1:1）", g.client_size)

        print("[3/7] 截图口径")
        p_boot, size = g.shot(os.path.join(out, "01_boot.png"))
        check(size == (E.GBA_W, E.GBA_H), "客户区 PNG = 240x160", size)

        print("[4/7] 预热（让模拟器跑过开机 logo）")
        time.sleep(args.warmup)
        print("      已等 %.1fs" % args.warmup)

        print("[5/7] 画面在动（定时器在跑）+ 有真画面的帧")
        shots = []
        for i in range(6):
            p, _ = g.shot(os.path.join(out, "02_idle_%02d.png" % i))
            shots.append(E.read_png(p))
            time.sleep(0.35)
        diffs = [E.diff_ratio(shots[i], shots[i + 1]) for i in range(len(shots) - 1)]
        check(max(diffs) > 0.001, "相邻帧差异最大值 > 0.1%", ["%.4f" % d for d in diffs])
        # ⚠ "非黑比例"只当**内容**的粗略指标：开机 logo 那几帧本来就是大片纯黑/纯白
        #   （实测 Nintendo logo 帧 nonblack 只有 1%）。所以判的是"这批帧里至少有一帧
        #   不是单色块"，而不是"每一帧都要够亮"。
        best = max(shots, key=lambda im: E.nonblack_ratio(im))
        nb, colors = E.nonblack_ratio(best), E.unique_colors(best)
        check(nb > 0.05 or colors >= 4, "存在非单色的画面帧（ROM 真的在渲染）",
              "最好的一帧: nonblack=%.3f 颜色数=%d" % (nb, colors))

        print("[6/7] 帧数在涨（状态文件自报）")
        s1 = g.game_status()
        time.sleep(0.6)
        s2 = g.game_status()
        print("      status: %s" % json.dumps(s2, ensure_ascii=False))
        check(s2.get("rom") is True, "gba.ts 自报 ROM 已打开（失败时 error 看上面那行）")
        check(int(s2.get("frames", 0)) > int(s1.get("frames", 0)),
              "frameCount 在涨", "%s -> %s" % (s1.get("frames"), s2.get("frames")))

        print("[7/7] 按键投递")
        keys = [k.strip() for k in (args.keys or "Right,Z,Return").split(",") if k.strip()]
        before, _ = g.shot(os.path.join(out, "03_before_keys.png"))
        for k in keys:
            r = g.press(k, hold_ms=200)
            handled = bool((r.get("press") or {}).get("handled")) or \
                      bool((r.get("release") or {}).get("handled"))
            check(handled, "按键 %s 投进了控件树 (handled)" % k,
                  (r.get("press") or {}).get("window"))
        time.sleep(0.5)
        after, _ = g.shot(os.path.join(out, "04_after_keys.png"))
        d = E.diff_ratio(E.read_png(before), E.read_png(after))
        print("      [info] 按键前后帧差 = %.4f（有差异 ≠ 游戏响应了，得看图判断）" % d)
    finally:
        if not args.keep:
            g.close()
        else:
            print("== --keep: 容器还在跑（port=%s）" % (g.proc.port if g.proc else "?"))

    print("== %s ==" % ("PASS" if not _fail else ("FAIL(%d)" % len(_fail))))
    if _fail:
        for m in _fail:
            print("  - %s" % m)
    print("== 图像证据: %s（boot / idle / before / after 各一张，人眼确认游戏画面）==" % out)
    return 0 if not _fail else 1


if __name__ == "__main__":
    sys.exit(main())
