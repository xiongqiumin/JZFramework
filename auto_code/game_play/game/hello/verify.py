#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
game/hello/verify.py —— **hello 玩法自己的尺子**（每个玩法一份，跟玩法放一起）

它自己就能跑完一局并判定，不依赖顶层驱动：

  ① 清空容器里**所有**程序（GET /api/programs → 逐个 DELETE）—— 保证这一局是干净的；
  ② 装 game（game.js）→ 等它的窗口；
  ③ 装 play（play.js）→ 让它自己玩；
  ④ 一边跑一边**从外面抓帧**（客观旁证：external/ + metrics.jsonl），并读**状态文件**
     （score/lives/over/win —— 游戏自己每 500ms 写一次，见 game.ts 的"状态上报"）；
  ⑤ 判定：cleared(通关) / game_over(被打死) / program_gone(程序没了) / running(到点还在跑)；
  ⑥ 落盘 result.json + report.md；**退出码 0 = 通关**（其余都是没通关）。

用法：
  python verify.py                                  # 自包含：自己起一个无头容器，跑完收掉
  python verify.py --port 60180                     # 用**已有**容器（顶层 run.py 起的那个）
  python verify.py --play-js <路径> --game-js <路径>  # 指定产物（缺省按玩法配置推）
  python verify.py --steps 200 --step-ms 200        # 观察节奏（缺省读玩法配置）

⚠ 状态文件是**尺子的特权**：脚本(play.ts)按红线**不许读**它（见任务书 prompt.md）。
"""

import argparse
import json
import os
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))            # game/hello
TASK = os.path.dirname(os.path.dirname(HERE))                # 任务目录
BUILD = os.path.join(TASK, "build")
CUR = os.path.join(TASK, "runs", "current")
sys.path.insert(0, TASK)
import env as E  # noqa: E402


def log(msg):
    print("[verify:hello] %s" % msg)


def read_status(path):
    """读游戏写的状态文件（还没写/写一半都可能，所以容错）。"""
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return None


def main():
    ap = argparse.ArgumentParser(description="hello 玩法的尺子：清容器 → 装 game+play → 观察 → 判定")
    ap.add_argument("--play", default="hello", help="玩法名（task.json 的 plays）")
    ap.add_argument("--port", type=int, default=0, help="已有容器的 HTTP 端口（0 = 自己起一个）")
    ap.add_argument("--out", default=CUR, help="产物目录（默认 runs/current）")
    ap.add_argument("--game-js", default="", help="题目侧产物（默认 build/<玩法配置的 js>）")
    ap.add_argument("--play-js", default="", help="脚本产物（默认 build/play/play.js）")
    ap.add_argument("--steps", type=int, default=0, help="观察多少拍（0 = 读玩法配置）")
    ap.add_argument("--step-ms", type=int, default=0, help="每拍间隔（0 = 读玩法配置）")
    ap.add_argument("--warmup", type=float, default=-1.0, help="装完脚本后先等多久（-1 = 读配置）")
    ap.add_argument("--seconds", type=float, default=0.0, help="按时间收工（脚本参数）")
    ap.add_argument("--target", type=int, default=0, help="通关线（0 = 读玩法配置）")
    ap.add_argument("--keep", action="store_true", help="自己起的容器测完不杀（手工接着看）")
    args = ap.parse_args()

    cfg = E.play_config(args.play, base_dir=TASK)
    steps = args.steps or int(cfg.get("steps", 120))
    step_ms = args.step_ms or int(cfg.get("stepMs", 400))
    warmup = args.warmup if args.warmup >= 0 else float(cfg.get("warmupSec", 2.0))
    target = args.target or int(cfg.get("targetScore", 300))
    program = cfg["program"]
    game_js = args.game_js or os.path.join(BUILD, cfg["js"])
    play_js = args.play_js or os.path.join(BUILD, "play", "play.js")
    status_path = cfg.get("statusPath") or "runs/current/hello_status.json"
    if not os.path.isabs(status_path):
        status_path = os.path.join(TASK, status_path)

    for p, why in ((game_js, "题目侧产物(game.js)"), (play_js, "脚本产物(play.js)")):
        if not os.path.exists(p):
            print("FAIL: 找不到 %s: %s（先编译：tsc -p tsconfig.json）" % (why, p))
            return 1

    out = args.out
    ext_dir = os.path.join(out, "external")
    metrics_path = os.path.join(out, "metrics.jsonl")
    os.makedirs(out, exist_ok=True)
    shutil.rmtree(ext_dir, ignore_errors=True)
    os.makedirs(ext_dir, exist_ok=True)
    for f in ("metrics.jsonl", "result.json", "report.md", "session.json"):
        p = os.path.join(out, f)
        if os.path.exists(p):
            try:
                os.remove(p)
            except Exception:
                open(p, "w").close()      # 删不掉就清空（本机删除会走回收站）

    own_container = (args.port == 0)
    box = None
    started = time.time()
    frames = 0
    status = None
    verdict = "unknown"
    detail = ""
    try:
        # ---- 0) 容器：外部给的 or 自己起一个 ----
        if own_container:
            box = E.HeadlessContainer(cfg="Debug", desktop=cfg["desktop"],
                                      log_path=os.path.join(out, "container_stdout.txt"))
            box.__enter__()
            conn = box.c
            log("自起容器: %s（桌面 %s）" % (conn.base, cfg["desktop"]))
        else:
            conn = E.Container(args.port)
            log("用已有容器: %s" % conn.base)

        # ---- ① 清空容器里所有程序 ----
        try:
            names = [p.get("name") for p in conn.programs()]
        except Exception as ex:
            print("FAIL: 容器不可用（%s）: %r" % (conn.base, ex))
            return 1
        for name in names:
            log("清掉容器里的程序: %s" % name)
            try:
                conn.stop_program(name)
            except Exception as ex:
                log("  （没清掉: %r）" % ex)
        if names:
            time.sleep(0.5)

        # ---- ② 装 game（题目侧）----
        E.write_game_config(os.path.dirname(game_js), cfg.get("dllDir", ""), cfg.get("rom", ""),
                            status_path=status_path, extra={"targetScore": target})
        log("装游戏: %s（进程名 %s）" % (os.path.relpath(game_js, TASK), program))
        conn.load_program(program, game_js, work_dir=os.path.dirname(game_js))
        win = conn.wait_window(program, timeout=60)
        if not win:
            print("FAIL: 游戏窗口没出来（%s 起不来？看 container_stdout.txt）" % program)
            return 1
        client = win.get("client") or [0, 0, 0, 0]
        log("游戏窗口: title=%s client=%s" % (win.get("title"), client))

        # ---- ③ 装 play（产出物）----
        E.write_play_config(BUILD, out, window=cfg["window"], warmup_sec=warmup, steps=steps,
                            step_ms=step_ms, max_seconds=args.seconds, app=program, play=cfg["play"])
        log("装脚本: %s（预热 %ss）" % (os.path.relpath(play_js, TASK), warmup))
        conn.load_program(E.PLAY_PROGRAM, play_js, work_dir=os.path.dirname(play_js))

        # ---- ④ 一边跑一边抓帧 + 读状态 ----
        session_path = os.path.join(out, "session.json")
        budget = (args.seconds if args.seconds > 0 else steps * step_ms / 1000.0) + warmup + 30.0
        t_end = time.time() + budget
        script_done_at = 0.0
        prev_img = None
        log("观察中（最多 %.0fs；通关线 %d 分）…" % (budget, target))
        while time.time() < t_end:
            status = read_status(status_path)
            if status:
                if status.get("win"):
                    verdict, detail = "cleared", "通关：score=%s" % status.get("score")
                    break
                if status.get("over"):
                    verdict, detail = "game_over", "被打死：score=%s" % status.get("score")
                    break
            try:
                progs = {p.get("name") for p in conn.programs()}
                if program not in progs:
                    verdict, detail = "program_gone", "游戏进程不在了"
                    break
                if E.PLAY_PROGRAM not in progs:
                    verdict, detail = "script_gone", "脚本进程退出了"
                    break
                p = os.path.join(ext_dir, "e%05d.png" % frames)
                with open(p, "wb") as f:
                    f.write(conn.screen_png(win.get("id")))
                img = E.read_png(p)
                m = {"i": frames, "tMs": int((time.time() - started) * 1000),
                     "file": os.path.relpath(p, TASK), "w": img[0], "h": img[1],
                     "colors": E.unique_colors(img), "nonblack": round(E.nonblack_ratio(img), 4),
                     "drift": round(E.diff_ratio(prev_img, img), 4) if prev_img else 0.0,
                     "score": (status or {}).get("score")}
                prev_img = img
                with open(metrics_path, "a", encoding="utf-8") as f:
                    f.write(json.dumps(m, ensure_ascii=False) + "\n")
                frames += 1
            except Exception as ex:
                log("抓帧失败: %r" % ex)
                time.sleep(0.5)
            if script_done_at == 0.0 and os.path.exists(session_path) \
                    and os.path.getsize(session_path) > 0:
                script_done_at = time.time() + 8.0     # 脚本收工后再给 8s 宽限（游戏可能还在跑）
                log("脚本已收工，再观察 8s")
            if script_done_at and time.time() > script_done_at:
                verdict, detail = "running", "脚本已收工但还没通关"
                break
            time.sleep(max(0.05, step_ms / 1000.0))
        else:
            verdict, detail = "running", "到点还在跑（没通关也没死）"
    finally:
        if own_container and box is not None and not args.keep:
            box.__exit__(None, None, None)
            log("容器已收掉")

    passed = (verdict == "cleared")
    result = {
        "play": cfg["play"], "verdict": verdict, "pass": passed, "detail": detail,
        "target": target, "status": status or {}, "externalFrames": frames,
        "elapsedSec": round(time.time() - started, 2),
        "container": {"program": program, "window": cfg["window"], "native": cfg["native"],
                      "desktop": cfg["desktop"], "own": own_container},
        "ts": int(time.time()),
    }
    with open(os.path.join(out, "result.json"), "w", encoding="utf-8") as f:
        json.dump(result, f, ensure_ascii=False, indent=2)

    # ---- ⑥ 一页摘要（给人/给 agent 看）----
    best = ""
    if frames:
        best = os.path.relpath(os.path.join(ext_dir, "e%05d.png" % max(0, frames // 2)), TASK)
    lines = ["# hello 一局判定", "",
             "- 结论: **%s**（%s）" % (verdict, detail),
             "- 通关线: %d 分；状态: %s" % (target, json.dumps(status or {}, ensure_ascii=False)),
             "- 观察 %d 帧 / 用时 %.1fs" % (frames, result["elapsedSec"]),
             "- 容器: 进程 %s，窗口 %s，桌面 %s%s"
             % (program, cfg["window"], cfg["desktop"], "（自起）" if own_container else "（外部给）"),
             ""]
    if best:
        lines += ["关键帧: ![f](%s)" % best, ""]
    with open(os.path.join(out, "report.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines))

    print("== hello 判定: %s（%s）｜通关线 %d｜观察 %d 帧｜用时 %.1fs"
          % (verdict, detail, target, frames, result["elapsedSec"]))
    print("PASS" if passed else "FAIL")
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
