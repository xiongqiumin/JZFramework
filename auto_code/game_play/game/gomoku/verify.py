#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
game/gomoku/verify.py —— **gomoku 玩法自己的尺子**（每个玩法一份，跟玩法放一起）

它自己就能跑完一局并判定，不依赖顶层驱动：

  ① 清空容器里**所有**程序（GET /api/programs → 逐个 DELETE）—— 保证这一局是干净的；
  ② 装 game（本体 = binary/code_agent/runtime/gomoku 的 TS 工程产物 build/main.js，jsRoot=repo
     并在配置的 prebuild 里编译）→ 等它的窗口；
  ③ 装 play（play.js）→ 让它自己下；
  ④ 一边跑一边**从外面抓帧**（客观旁证：external/ + metrics.jsonl），并读**容器日志里本体
     自己打印的事件行**（容器把程序 stdout 带 [vproc: "gomoku"] 前缀转发出来）：
        [gomoku] start grid=15 cell=40 board=600x600 …      这局开张了
        [gomoku] human move r=.. c=.. no=..                  玩家(黑)落子
        [gomoku] ai move r=.. c=.. no=..                     电脑(白)落子
        [gomoku] RESULT=human_win|ai_win|draw …              谁赢了（+ 终局棋盘 15 行 board NN ...）
  ⑤ 判定（**这个玩法要"赢"**）：
        cleared     = RESULT=human_win，且在 limitSec(缺省 180s) 内  ⇒ **退出码 0 = 这局算过**
        game_over   = RESULT=ai_win（电脑赢了）
        draw        = RESULT=draw（平局）
        timeout     = 到 180s 还没分出胜负（没下赢）
        script_done = 脚本自己收工了（写了 session.json）但没下赢
        no_report   = 整局没有任何 [gomoku] 事件（本体没跑起来？）
        program_gone / script_gone = 进程没了
  ⑥ 落盘 result.json + report.md（含终局棋盘）；退出码 0 = 通关。

用法：
  python verify.py                                  # 自包含：自己起一个无头容器，跑完收掉
  python verify.py --port 60180                     # 用**已有**容器（顶层 run.py 起的那个）
  python verify.py --play-js <路径> --game-js <路径>  # 指定产物（缺省按玩法配置推）
  python verify.py --limit 60                       # 换限时（秒；缺省读玩法配置 limitSec=180）
  python verify.py --keep                           # 自己起的容器测完不杀（手工接着看）

⚠ 那些事件行是**尺子的特权**：脚本(play.ts)按红线**不许读**容器日志、不许读本体源码（见任务书 prompt.md）。
"""

import argparse
import json
import os
import re
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))            # game/gomoku
TASK = os.path.dirname(os.path.dirname(HERE))                # 任务目录
BUILD = os.path.join(TASK, "build")
CUR = os.path.join(TASK, "runs", "current")
sys.path.insert(0, TASK)
import env as E  # noqa: E402


def log(msg):
    print("[verify:gomoku] %s" % msg)


def read_events(conn, offset=0, prefix="[gomoku]"):
    """从**容器**拉本体的程序输出（GET /api/logs），解析里面的 [gomoku] 事件行。

    offset: 起始**绝对行号**（累计行号）—— 同一个容器跑多轮时日志会累积，从 0 读会把上一局的
    输赢算进来，所以调用方先取一次 total 当基线（见 main 里的 log_from）。
    返回 {seen, started, human, ai, restarts, result, board, lines, total}；
    seen=False = 这一段里没打印过（本体没跑起来，或它根本没打印）。
    """
    out = {"seen": False, "started": False, "human": 0, "ai": 0, "restarts": 0,
           "result": "", "board": [], "lines": [], "total": None}
    try:
        d = conn.logs(offset=offset, size=-1)
    except Exception:
        return out                       # 容器正好在忙/没起来: 当这一拍没读到, 下一拍再看
    out["total"] = d.get("total")
    for line in (d.get("lines") or []):
        # 行文本 = "[vproc: <进程名>] <原始输出>"; 只认本体打印的那几行(带 [gomoku] 前缀)
        i = line.find(prefix)
        if i < 0:
            continue
        s = line[i + len(prefix):].strip()
        if not s:
            continue
        out["seen"] = True
        out["lines"].append(s)
        if s.startswith("start"):
            out["started"] = True
        elif s.startswith("human move"):
            out["human"] += 1
        elif s.startswith("ai move"):
            out["ai"] += 1
        elif s.startswith("restart"):
            out["restarts"] += 1
        m = re.search(r"RESULT=(\w+)", s)
        if m:
            out["result"] = m.group(1)
        m = re.match(r"board\s+(\d+)\s+([.XO]{5,})", s)
        if m:
            out["board"].append(m.group(2))
    return out


def main():
    ap = argparse.ArgumentParser(description="gomoku 玩法的尺子：清容器 → 装 game+play → 观察 → 判定（要下赢）")
    ap.add_argument("--play", default="gomoku", help="玩法名（game/<名>/config.json）")
    ap.add_argument("--port", type=int, default=0, help="已有容器的 HTTP 端口（0 = 自己起一个）")
    ap.add_argument("--out", default=CUR, help="产物目录（默认 runs/current）")
    ap.add_argument("--game-js", default="", help="题目侧产物（默认按玩法配置推；本玩法 jsRoot=repo）")
    ap.add_argument("--play-js", default="", help="脚本产物（默认 build/play/play.js）")
    ap.add_argument("--steps", type=int, default=0, help="观察多少拍（0 = 读玩法配置）")
    ap.add_argument("--step-ms", type=int, default=0, help="每拍间隔（0 = 读玩法配置）")
    ap.add_argument("--warmup", type=float, default=-1.0, help="装完脚本后先等多久（-1 = 读配置）")
    ap.add_argument("--seconds", type=float, default=0.0, help="脚本自己的收工时间（0 = 用限时 limit）")
    ap.add_argument("--limit", type=float, default=0.0, help="**判定的限时**（秒；0 = 读玩法配置 limitSec）")
    ap.add_argument("--target", type=int, default=0, help="（本玩法不用；兼容驱动的通用参数）")
    ap.add_argument("--keep", action="store_true", help="自己起的容器测完不杀（手工接着看）")
    args = ap.parse_args()

    cfg = E.play_config(args.play, base_dir=TASK)
    steps = args.steps or int(cfg.get("steps", 900))
    step_ms = args.step_ms or int(cfg.get("stepMs", 1000))
    warmup = args.warmup if args.warmup >= 0 else float(cfg.get("warmupSec", 1.5))
    limit = args.limit or float(cfg.get("limitSec", 180))
    script_seconds = args.seconds if args.seconds > 0 else limit      # 脚本自己按这个收工
    prefix = str(cfg.get("report") or "[gomoku]")
    program = cfg["program"]
    game_js = args.game_js or E.play_js_path(cfg, TASK, BUILD)
    play_js = args.play_js or os.path.join(BUILD, "play", "play.js")

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
    t_start = time.time()          # 尺子起跑（含起容器）
    t_load = 0.0                   # 脚本装上的那一刻 —— **3 分钟从这儿算**
    frames = 0
    report = {"seen": False, "started": False, "human": 0, "ai": 0, "restarts": 0,
              "result": "", "board": [], "lines": []}
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
        # 不写运行根的 game.json：本体只有硬编码常量 + 打印，不落状态文件、也不读它（见文件头）
        log("装游戏: %s（进程名 %s）" % (os.path.relpath(game_js, TASK), program))
        conn.load_program(program, game_js, work_dir=os.path.dirname(game_js))
        win = conn.wait_window(program, timeout=60)
        if not win:
            print("FAIL: 游戏窗口没出来（%s 起不来？看 container_stdout.txt）" % program)
            return 1
        client = win.get("client") or [0, 0, 0, 0]
        log("游戏窗口: title=%s client=%s" % (win.get("title"), client))

        # ---- ③ 装 play（产出物）----
        # play.json 放在**脚本产物所在目录**（脚本按 jzApp.applicationDirPath() 找它）
        E.write_play_config(os.path.dirname(play_js), out, window=cfg["window"], warmup_sec=warmup,
                            steps=steps, step_ms=step_ms, max_seconds=script_seconds, app=program,
                            play=cfg["play"])
        log("装脚本: %s（预热 %ss，脚本自己的限时 %ss）"
            % (os.path.relpath(play_js, TASK), warmup, script_seconds))
        conn.load_program(E.PLAY_PROGRAM, play_js, work_dir=os.path.dirname(play_js))
        t_load = time.time()

        # ---- ④ 一边跑一边抓帧 + 读本体事件 ----
        session_path = os.path.join(out, "session.json")
        # 容器侧的日志缓冲会**跨轮累积**（同一个容器一直在用）：先记下当前累计行号当基线
        try:
            log_from = conn.logs().get("total") or 0
        except Exception:
            log_from = 0
        report = read_events(conn, log_from, prefix)
        t_end = t_load + limit + 3.0        # 3 分钟（+3s 给日志转发留点余量）
        script_done_at = 0.0
        prev_img = None
        log("观察中（限时 %.0fs：要么看到 RESULT=human_win，要么超时）…" % limit)
        while time.time() < t_end:
            # 读事件：一有 RESULT 就再补读一次（把终局棋盘那 15 行也捞回来）再收工
            report = read_events(conn, log_from, prefix)
            if report["result"]:
                time.sleep(0.5)
                report = read_events(conn, log_from, prefix)
                break
            if report["restarts"] and report["ai"] == 0 and report["human"] == 0:
                pass                        # 有人点了"重新开始"（不影响判定，只记着）
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
                m = {"i": frames, "tMs": int((time.time() - t_start) * 1000),
                     "file": os.path.relpath(p, TASK), "w": img[0], "h": img[1],
                     "colors": E.unique_colors(img), "nonblack": round(E.nonblack_ratio(img), 4),
                     "drift": round(E.diff_ratio(prev_img, img), 4) if prev_img else 0.0,
                     "human": report["human"], "ai": report["ai"], "result": report["result"]}
                prev_img = img
                with open(metrics_path, "a", encoding="utf-8") as f:
                    f.write(json.dumps(m, ensure_ascii=False) + "\n")
                frames += 1
            except Exception as ex:
                log("抓帧失败: %r" % ex)
                time.sleep(0.5)
            if script_done_at == 0.0 and os.path.exists(session_path) \
                    and os.path.getsize(session_path) > 0:
                script_done_at = time.time() + 8.0     # 脚本收工后再给 8s 宽限（本体可能还在跑）
                log("脚本已收工，再观察 8s")
            if script_done_at and time.time() > script_done_at:
                verdict, detail = "script_done", "脚本收工了但没下赢（黑 %d 手 / 白 %d 手）" \
                    % (report["human"], report["ai"])
                break
            time.sleep(max(0.05, step_ms / 1000.0))
        else:
            verdict, detail = "timeout", "到 %gs 还没分出胜负（黑 %d 手 / 白 %d 手）" \
                % (limit, report["human"], report["ai"])

        # ---- ⑤ 判定（这一节是这个玩法自己的口径）----
        if not verdict or verdict == "unknown":
            if report["result"] == "human_win":
                verdict = "cleared"
                detail = "3 分钟内下赢：黑棋连五（黑 %d 手 / 白 %d 手）" % (report["human"], report["ai"])
            elif report["result"] == "ai_win":
                verdict, detail = "game_over", "电脑赢了（黑 %d 手 / 白 %d 手）" % (report["human"], report["ai"])
            elif report["result"] == "draw":
                verdict, detail = "draw", "平局（棋盘下满）"
            elif not report["seen"]:
                verdict, detail = "no_report", "整局没有任何 %s 事件行（本体没跑起来？看 container_stdout.txt）" % prefix
            else:
                verdict, detail = "timeout", "到 %gs 还没分出胜负（黑 %d 手 / 白 %d 手）" \
                    % (limit, report["human"], report["ai"])
    finally:
        if own_container and box is not None and not args.keep:
            box.__exit__(None, None, None)
            log("容器已收掉")

    passed = (verdict == "cleared")
    elapsed = round(time.time() - t_load, 2) if t_load else 0.0
    result = {
        "play": cfg["play"], "verdict": verdict, "pass": passed, "detail": detail,
        "limitSec": limit, "scriptSeconds": script_seconds,
        "elapsedSec": elapsed,                     # 从**脚本装上**到判定（3 分钟从这儿算）
        "rulerTotalSec": round(time.time() - t_start, 2),
        "report": {"seen": report["seen"], "started": report["started"],
                   "human": report["human"], "ai": report["ai"], "restarts": report["restarts"],
                   "result": report["result"], "board": report["board"], "lines": len(report["lines"])},
        "externalFrames": frames,
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
    lines = ["# gomoku 一局判定", "",
             "- 结论: **%s**（%s）" % (verdict, detail),
             "- 限时: %gs（脚本装上后计时）｜用时 %.1fs｜黑 %d 手 / 白 %d 手｜重来 %d 次"
             % (limit, elapsed, report["human"], report["ai"], report["restarts"]),
             "- 本体打印的终局: `RESULT=%s`（%d 行事件）" % (report["result"] or "(无)", len(report["lines"])),
             "- 本体打印的棋盘: `%s`" % ("|".join(report["board"]) if report["board"] else "(没有)"),
             "- 观察 %d 帧｜容器: 进程 %s，窗口 %s，桌面 %s%s"
             % (frames, program, cfg["window"], cfg["desktop"], "（自起）" if own_container else "（外部给）"),
             ""]
    if best:
        lines += ["关键帧: ![f](%s)" % best, ""]
    if report["board"]:
        lines += ["```", "   " + "".join("%X" % (i % 16) for i in range(len(report["board"][0])))] \
            + ["%2d %s" % (i, row) for i, row in enumerate(report["board"])] + ["```", ""]
    with open(os.path.join(out, "report.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines))

    print("== gomoku 判定: %s（%s）｜限时 %gs｜用时 %.1fs｜黑 %d 手 / 白 %d 手"
          % (verdict, detail, limit, elapsed, report["human"], report["ai"]))
    print("PASS" if passed else "FAIL")
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
