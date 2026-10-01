#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gba 任务 —— 评测驱动（一轮 = 编译 → 在容器里跑 play.ts 一局 → 判分 → 归档）

角色（别混）：
  · `play/play.ts` —— **最终产出**：容器里跑的一键通关脚本，用 computer use 自己玩；
  · `game/game.ts` —— **题目侧**：容器里的"游戏机"（只开窗 + 跑 ROM，没有任何策略）；
  · 本文件         —— **驱动**：起容器 → 装 game → 装 play → 从**外面**抓帧（客观旁证）
                       → 等脚本收工 → 判分（verify.py）→ 归档 runs/round_NNN。

为什么外面还要抓一份帧：脚本自己留的帧/日志是"它说它看到了什么"，外面这份是**客观证据**
（`GET /api/screen?window=<游戏窗口>` 抓的就是那块屏）。两边对得上，"脚本真的在玩"才不是自说自话。

用法（本目录下）:
  python run.py                        # 一轮（默认 120 拍、每拍 400ms、预热 3s）
  python run.py --seconds 300          # 长局（按时间收工；脚本自己也按同一套参数收工）
  python run.py --steps 0 --seconds 600
  python run.py --skip-build           # TS 没改就跳过 tsc
  python run.py --no-archive           # 只跑 + 判，不归档

产物在 runs/current/：play.log / frames/（脚本留的）＋ external/ + metrics.jsonl（驱动抓的）
＋ session.json（脚本自报）＋ container_stdout.txt / play_stdout.txt（两侧日志）。
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import time

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
BUILD = os.path.join(HERE, "build")
RUNS = os.path.join(HERE, "runs")
CUR = os.path.join(RUNS, "current")
sys.path.insert(0, HERE)
import env as E  # noqa: E402

DEFAULT_ROM = os.path.join("E:/work/simulator", "火焰之纹章-圣魔之光石完美修改版.gba")


def run(cmd, cwd=None, check=True, timeout=3600):
    print("+ " + " ".join(cmd))
    p = subprocess.run(cmd, cwd=cwd, capture_output=True, timeout=timeout)
    out = (p.stdout or b"").decode("utf-8", "replace") + (p.stderr or b"").decode("utf-8", "replace")
    if out.strip():
        print(out.rstrip())
    if check and p.returncode != 0:
        print("FAIL: 命令返回 %d" % p.returncode)
        sys.exit(p.returncode)
    return p.returncode, out


def fresh_file(path):
    if os.path.exists(path):
        os.remove(path)


def round_index():
    if not os.path.isdir(RUNS):
        return 0
    return len([d for d in os.listdir(RUNS) if d.startswith("round_")])


def main():
    ap = argparse.ArgumentParser(description="gba 评测驱动：容器里跑 play.ts 一局 → 判分 → 归档")
    ap.add_argument("--cfg", default="Debug")
    ap.add_argument("--rom", default=DEFAULT_ROM)
    ap.add_argument("--dll-dir", default="")
    ap.add_argument("--desktop", default="320x240")
    ap.add_argument("--steps", type=int, default=120, help="脚本最多跑多少拍（0 = 只看 --seconds）")
    ap.add_argument("--seconds", type=float, default=0.0, help="本局最多多少秒（0 = 只看拍数）")
    ap.add_argument("--step-ms", type=int, default=400, help="脚本每拍间隔（也是外面抓帧的间隔）")
    ap.add_argument("--warmup", type=float, default=3.0, help="脚本激活窗口后先等多久（过开机 logo）")
    ap.add_argument("--timeout", type=float, default=0.0,
                    help="驱动最多等脚本多久（秒；0 = 按 steps/seconds 自动算 + 60s 余量）")
    ap.add_argument("--skip-build", action="store_true")
    ap.add_argument("--no-archive", action="store_true")
    args = ap.parse_args()

    tsc = E.tsc_exe(args.cfg)
    exe = E.container_exe(args.cfg)
    dll_dir = args.dll_dir or E.emulator_dll_dir(args.cfg)
    for p, why in ((exe, "容器 virtual_device.exe"), (tsc, "tsc"),
                   (os.path.join(dll_dir, "mgba_host.dll"), "mgba_host.dll"), (args.rom, "ROM")):
        if not os.path.exists(p):
            print("FAIL: 找不到 %s: %s" % (why, p))
            return 1

    # ---- 1) 编译：题目侧 game.ts + 产出物 play.ts ----
    if not args.skip_build:
        run([tsc, "-p", os.path.join(HERE, "tsconfig.json")], cwd=HERE)
    game_js = os.path.join(BUILD, "game", "game.js")
    play_js = os.path.join(BUILD, "play", "play.js")
    for p, why in ((game_js, "题目侧 game.js"), (play_js, "产出物 play.js（play/play.ts 编译产物）")):
        if not os.path.exists(p):
            print("FAIL: 找不到 %s: %s（先跑一次 tsc？）" % (why, p))
            return 1

    # ---- 2) 清产物目录（这一局的证据全在 runs/current）----
    frames_dir = os.path.join(CUR, "frames")       # 脚本自己留的帧
    ext_dir = os.path.join(CUR, "external")        # 驱动从外面抓的帧（客观旁证）
    metrics_path = os.path.join(CUR, "metrics.jsonl")
    for d in (frames_dir, ext_dir):
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d, exist_ok=True)
    for f in ("session.json", "play.log", "metrics.jsonl", "play_stdout.txt",
              "container_stdout.txt", "game_status.json", "result.json", "report.md"):
        fresh_file(os.path.join(CUR, f))
    os.makedirs(CUR, exist_ok=True)

    started = time.time()
    drive = {"rom": args.rom, "desktop": args.desktop, "steps": args.steps,
             "stepMs": args.step_ms, "warmupSec": args.warmup, "seconds": args.seconds,
             "externalFrames": 0, "externalSeconds": 0.0, "window": None, "clientSize": None,
             "scriptDone": False, "errors": []}

    try:
        with E.HeadlessContainer(cfg=args.cfg, desktop=args.desktop, exe=exe,
                                 log_path=os.path.join(CUR, "container_stdout.txt")) as box:
            # ---- 3) 题目侧：游戏机（窗口 240x160 + ROM），由驱动装 ----
            box.load(E.GAME_PROGRAM, game_js)
            win = box.c.wait_window(E.GAME_PROGRAM, timeout=60)
            client = win.get("client") or [0, 0, 0, 0]
            drive["window"] = win.get("title")
            drive["clientSize"] = [int(client[2]), int(client[3])]
            print("== 游戏窗口: title=%s client=%s 句柄=%s"
                  % (win.get("title"), client, win.get("id")))

            # ---- 4) 产出物：把参数写给 play.ts，然后把它装进同一个容器 ----
            cfg_path = E.write_play_config(BUILD, CUR, window=E.WINDOW_TITLE,
                                           warmup_sec=args.warmup, steps=args.steps,
                                           step_ms=args.step_ms, max_seconds=args.seconds)
            print("== play.ts 参数: %s（%s）"
                  % (os.path.relpath(cfg_path, ROOT),
                     json.dumps(E.read_json(cfg_path, {}), ensure_ascii=False)))
            log = open(os.path.join(CUR, "play_stdout.txt"), "w", encoding="utf-8")
            box.load(E.PLAY_PROGRAM, play_js)
            names = [p.get("name") for p in box.c.programs()]
            drive["programs"] = names
            print("== 容器里的进程: %s（%s=游戏机，%s=脚本）—— 从现在起脚本自己玩"
                  % (names, E.GAME_PROGRAM, E.PLAY_PROGRAM))

            # ---- 5) 外面抓帧（客观旁证）：脚本在玩的时候，驱动一直从外面看这块屏 ----
            session_path = os.path.join(CUR, "session.json")
            budget = args.timeout if args.timeout > 0 else \
                ((args.seconds if args.seconds > 0 else args.steps * args.step_ms / 1000.0)
                 + args.warmup + 60.0)
            t_end = time.time() + budget
            prev_img, n = None, 0
            while time.time() < t_end:
                if os.path.exists(session_path):
                    drive["scriptDone"] = True
                    break
                try:
                    p = os.path.join(ext_dir, "e%05d.png" % n)
                    with open(p, "wb") as f:
                        f.write(box.c.screen_png(win.get("id")))
                    img = E.read_png(p)
                    m = {"i": n, "tMs": int((time.time() - started) * 1000),
                         "file": os.path.relpath(p, HERE),
                         "w": img[0], "h": img[1],
                         "colors": E.unique_colors(img),
                         "nonblack": round(E.nonblack_ratio(img), 4),
                         "drift": round(E.diff_ratio(prev_img, img), 4) if prev_img else 0.0}
                    prev_img = img
                    with open(metrics_path, "a", encoding="utf-8") as f:
                        f.write(json.dumps(m, ensure_ascii=False) + "\n")
                    n += 1
                except Exception as ex:
                    drive["errors"].append("抓帧失败: %r" % ex)
                    time.sleep(0.5)
                time.sleep(max(0.05, args.step_ms / 1000.0))
            drive["externalFrames"] = n
            if n:
                drive["externalSeconds"] = round(time.time() - started, 2)
            if not drive["scriptDone"]:
                drive["errors"].append("等脚本收工超时（%.0fs 内没写出 session.json）" % budget)
                print("!! 等脚本收工超时 —— 它是不是卡住了？看 play_stdout.txt / container_stdout.txt")
            log.close()
    except Exception as ex:
        drive["errors"].append("驱动异常: %r" % ex)
        print("!! 驱动异常: %r" % ex)

    drive["elapsedSec"] = round(time.time() - started, 2)
    script = E.read_json(os.path.join(CUR, "session.json"), {}) or {}
    drive["script"] = {k: script.get(k) for k in ("reason", "steps", "elapsedMs",
                                                 "keys", "frames", "errors")}
    with open(os.path.join(CUR, "drive.json"), "w", encoding="utf-8") as f:
        json.dump(drive, f, ensure_ascii=False, indent=2)
    print("== 这一局: 外面抓帧 %d 张 / 用时 %.1fs / 脚本自报: %s"
          % (n, drive["elapsedSec"],
             ("steps=%s keys=%s reason=%s" % (script.get("steps"),
                                              len(script.get("keys") or []), script.get("reason")))
             if script else "（没有 session.json）"))

    # ---- 6) 判分 ----
    rc, out = run([sys.executable, os.path.join(HERE, "verify.py")], cwd=HERE, check=False)

    # ---- 7) 归档 ----
    if not args.no_archive:
        target = os.path.join(RUNS, "round_%03d" % (round_index() + 1))
        os.makedirs(target, exist_ok=True)
        for name in ("session.json", "drive.json", "metrics.jsonl", "result.json",
                     "play.log", "report.md", "container_stdout.txt", "play_stdout.txt"):
            src = os.path.join(CUR, name)
            if os.path.exists(src):
                shutil.copy2(src, os.path.join(target, name))
        # 脚本自己留的帧：关键张（每 5 拍 + 最后一张）
        sframes = sorted(f for f in os.listdir(frames_dir) if f.endswith(".png")) \
            if os.path.isdir(frames_dir) else []
        keep_s = sframes[::5] + ([sframes[-1]] if sframes and sframes[-1] not in sframes[::5] else [])
        # 外部帧：同样稀疏留（它们是 240x160，很轻，但别把上千张堆进归档）
        eframes = sorted(f for f in os.listdir(ext_dir) if f.endswith(".png")) \
            if os.path.isdir(ext_dir) else []
        keep_e = eframes[::5] + ([eframes[-1]] if eframes and eframes[-1] not in eframes[::5] else [])
        for sub, keep in (("frames", keep_s), ("external", keep_e)):
            os.makedirs(os.path.join(target, sub), exist_ok=True)
            for f in keep:
                shutil.copy2(os.path.join(CUR, sub, f), os.path.join(target, sub, f))
        with open(os.path.join(RUNS, "scores.jsonl"), "a", encoding="utf-8") as f:
            f.write(json.dumps({"round": os.path.basename(target),
                                "steps": script.get("steps"), "keys": len(script.get("keys") or []),
                                "externalFrames": n, "elapsedSec": drive["elapsedSec"],
                                "pass": ("PASS" in out.splitlines()), "ts": int(time.time())},
                               ensure_ascii=False) + "\n")
        print("== 归档 -> %s（看 report.md + frames/ + external/）==" % os.path.relpath(target, ROOT))

    print("== %s ==" % ("PASS" if rc == 0 else "FAIL"))
    return rc


if __name__ == "__main__":
    sys.exit(main())
