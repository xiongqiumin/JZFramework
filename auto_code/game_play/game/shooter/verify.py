#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
game/shooter/verify.py —— **shooter 玩法自己的尺子**（每个玩法一份，跟玩法放一起）

它自己就能跑完一局并判定，不依赖顶层驱动：

  ① 清空容器里**所有**程序（GET /api/programs → 逐个 DELETE）—— 保证这一局是干净的；
  ② 装 game（本体的 JS）→ 等它的窗口；
  ③ 装 play（play.js）→ 让它自己玩；
  ④ 一边跑一边**从外面抓帧**（客观旁证：external/ + metrics.jsonl），并读**容器日志里本体
     自己打印的状态行**（容器把程序 stdout 带 [vproc: "名字"] 前缀转发出来）：
        [shooter] score=.. lives=..        分数/命变化的那一刻
        [shooter] alive .. over=false      没变化时的心跳(2s 一行, 证明还活着)
        [shooter] GAME OVER score=..       游戏结束
  ⑤ 判定：cleared(打到 targetScore = 通关) / running(到点还在跑) / game_over(打到死) /
     program_gone(程序没了) / no_report(整局没打印＝没跑起来)。
     **通关线 = targetScore（默认 300 分；一架敌机 +10 ⇒ 30 架）**：本体打到目标分时会打
     "[shooter] CLEAR score=.. target=.."，尺子也认 score >= target（双保险）。本体不落
     状态文件，状态全靠它自己打印的那几行（见 ④）。
     ⚠ 通关后本体**自己把这一局停住**（世界定格、画面上盖"恭喜过关"）⇒ CLEAR 之后再抓的帧
       会几乎逐像素一样，那不是卡住。
  ⑥ 落盘 result.json + report.md；**退出码 0 = 这局算过**（通关，或至少真的跑起来了）。

用法：
  python verify.py                                  # 自包含：自己起一个无头容器，跑完收掉
  python verify.py --port 60180                     # 用**已有**容器（顶层 run.py 起的那个）
  python verify.py --play-js <路径> --game-js <路径>  # 指定产物（缺省按玩法配置推）
  python verify.py --steps 200 --step-ms 200        # 观察节奏（缺省读玩法配置）

⚠ 那条打印是**尺子的特权**：脚本(play.ts)按红线**不许读**它（见任务书 prompt.md）—— 脚本只能看画面。
"""

import argparse
import json
import os
import re
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))            # game/shooter
TASK = os.path.dirname(os.path.dirname(HERE))                # 任务目录
BUILD = os.path.join(TASK, "build")
CUR = os.path.join(TASK, "runs", "current")
sys.path.insert(0, TASK)
import env as E  # noqa: E402


def log(msg):
    print("[verify:shooter] %s" % msg)


def read_reports(conn, offset=0, prefix="[shooter]"):
    """从**容器**拉本体的程序输出（GET /api/logs），抓里面打印的状态行。

    offset: 起始**绝对行号**（累计行号）—— 同一个容器跑多轮时日志会累积，从 0 读会把上一局的
    分数算进来，所以调用方先取一次 total 当基线（见 main 里的 log_from）。
    返回 {seen, score, lives, over, alive, lines, total}；seen=False = 这一段里没打印过
    （程序没跑起来，或它根本没打印）。
    """
    out = {"seen": False, "score": None, "lives": None, "over": False, "alive": False,
           "clear": False, "lines": [], "total": None}
    try:
        d = conn.logs(offset=offset, size=-1)
    except Exception:
        return out                       # 容器正好在忙/没起来: 当这一拍没读到, 下一拍再看
    out["total"] = d.get("total")
    for line in (d.get("lines") or []):
        # 行文本 = "[vproc: <进程名>] <原始输出>"; 只认本体打印的那几行(带 [shooter] 前缀)
        i = line.find(prefix)
        if i < 0:
            continue
        s = line[i + len(prefix):].strip()
        if not s:
            continue
        out["seen"] = True
        out["lines"].append(s)
        ms = re.search(r"score=(\d+)", s)
        if ms:
            out["score"] = int(ms.group(1))
        ml = re.search(r"lives=(\d+)", s)
        if ml:
            out["lives"] = int(ml.group(1))
        if s.startswith("GAME OVER"):
            out["over"] = True
        if s.startswith("alive"):
            out["alive"] = True
        if s.startswith("CLEAR"):
            out["clear"] = True
    return out


def main():
    ap = argparse.ArgumentParser(description="shooter 玩法的尺子：清容器 → 装 game+play → 观察 → 判定")
    ap.add_argument("--play", default="shooter", help="玩法名（task.json 的 plays）")
    ap.add_argument("--port", type=int, default=0, help="已有容器的 HTTP 端口（0 = 自己起一个）")
    ap.add_argument("--out", default=CUR, help="产物目录（默认 runs/current）")
    ap.add_argument("--game-js", default="", help="题目侧产物（默认 build/<玩法配置的 js>）")
    ap.add_argument("--play-js", default="", help="脚本产物（默认 build/play/play.js）")
    ap.add_argument("--steps", type=int, default=0, help="观察多少拍（0 = 读玩法配置）")
    ap.add_argument("--step-ms", type=int, default=0, help="每拍间隔（0 = 读玩法配置）")
    ap.add_argument("--warmup", type=float, default=-1.0, help="装完脚本后先等多久（-1 = 读配置）")
    ap.add_argument("--seconds", type=float, default=0.0, help="按时间收工（脚本参数）")
    # ⚠ 自起容器**缺省用 Release** —— 这是"能不能量脚本"的问题，不是偏好:
    #   同一份脚本、同一台机器, 桌面合成在 Debug 下慢到脚本一次 matchAll 要 **1550ms**
    #   （Release 是 3~6ms, 差 300 倍）: 合成跟不上 ⇒ 脚本读到的是**过期画布** ⇒
    #   "看不见自己", 一局 0~10 分 —— 那条路上的分数量的是环境, 不是脚本。
    #   要复现某个评测环境就显式给 `--cfg Debug`（例: run.json 只是个示例, 别当权威）。
    ap.add_argument("--cfg", default="Release", help="自起容器用哪个构建（默认 Release；"
                                                     "Debug 下看屏脚本会瞎, 见上面的说明）")
    ap.add_argument("--poll-ms", type=int, default=0, dest="poll_ms",
                    help="尺子**自己**的轮询间隔（毫秒；0 = 跟脚本的 stepMs 一样）。"
                         "⚠ 轮询 = 容器侧的 HTTP（/api/logs、/api/programs），它们在容器里是"
                         "**同步跑在桌面主线程**上的一段活；而脚本的\"看一眼\"（matchAll）要的正是"
                         "同一个主线程 ⇒ 轮得越密，脚本越看不见。判分只需要读到本体打印的 "
                         "score 行（2s 一行的心跳足够），没必要 400ms 一次。")
    ap.add_argument("--ext-every-ms", type=int, default=0, dest="ext_every_ms",
                    help="外部旁证帧的间隔（毫秒；**默认 0 = 只在开局与判定时刻各抓一张**）。"
                         "⚠ 运行期连抓会把**被测对象**压住：抓一张 = 容器侧整窗合成/编码 + 跨线程"
                         "取帧，实测开着抓时画面从 ~10s 起**完全冻结**（帧字节逐一相同）、脚本变成"
                         "瞎子，同一份脚本同一容器 10~60 分 vs 关掉时 110 分。判分只读本体打印的 "
                         "score 行，旁证帧是给人看的 —— 所以默认只留关键帧；要更密的证据自己调小"
                         "（并接受那是在'压住脚本'的条件下测的）。")
    ap.add_argument("--target", type=int, default=0, help="通关线（0 = 读玩法配置的 targetScore）")
    ap.add_argument("--keep", action="store_true", help="自己起的容器测完不杀（手工接着看）")
    args = ap.parse_args()

    cfg = E.play_config(args.play, base_dir=TASK)
    steps = args.steps or int(cfg.get("steps", 120))
    step_ms = args.step_ms or int(cfg.get("stepMs", 400))
    warmup = args.warmup if args.warmup >= 0 else float(cfg.get("warmupSec", 2.0))
    target = args.target or int(cfg.get("targetScore", 300))
    program = cfg["program"]
    # 本体可能住在任务之外（本玩法 jsRoot="repo"，直接用 test_jsshooter 的产物）：见 E.play_js_path
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
    started = time.time()
    frames = 0
    status = None
    verdict = "unknown"
    detail = ""
    try:
        # ---- 0) 容器：外部给的 or 自己起一个 ----
        if own_container:
            cfgname = args.cfg
            box = E.HeadlessContainer(cfg=cfgname, desktop=cfg["desktop"],
                                      log_path=os.path.join(out, "container_stdout.txt"))
            box.__enter__()
            conn = box.c
            log("自起容器(%s): %s（桌面 %s）" % (cfgname, conn.base, cfg["desktop"]))
            if cfgname.lower() == "debug":
                log("⚠⚠ 容器是 **Debug** 构建：桌面合成慢到看屏脚本看不见画面（实测一次 matchAll "
                    "1550ms vs Release 3~6ms）—— 这一局的分数**量的是环境，不是脚本**。")
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
        # 不写运行根的 game.json：本体只打印、不落状态文件、也不读它（见文件头）
        log("装游戏: %s（进程名 %s）" % (game_js, program))
        conn.load_program(program, game_js, work_dir=os.path.dirname(game_js))
        win = conn.wait_window(program, timeout=60)
        if not win:
            print("FAIL: 游戏窗口没出来（%s 起不来？看 container_stdout.txt）" % program)
            return 1
        client = win.get("client") or [0, 0, 0, 0]
        log("游戏窗口: title=%s client=%s" % (win.get("title"), client))

        # ---- ③ 装 play（产出物）----
        # play.json 放在**脚本产物所在目录**（脚本按 jzApp.applicationDirPath() 找它）：
        # 产出物是 build/play/play.js，玩法自带的参考脚本在 build/game/<玩法>/play.js —— 各有各的根
        E.write_play_config(os.path.dirname(play_js), out, window=cfg["window"], warmup_sec=warmup,
                            steps=steps, step_ms=step_ms, max_seconds=args.seconds, app=program,
                            play=cfg["play"])
        log("装脚本: %s（预热 %ss）" % (os.path.relpath(play_js, TASK), warmup))
        conn.load_program(E.PLAY_PROGRAM, play_js, work_dir=os.path.dirname(play_js))

        # ---- ④ 一边跑一边抓帧 + 读本体打印 ----
        session_path = os.path.join(out, "session.json")
        budget = (args.seconds if args.seconds > 0 else steps * step_ms / 1000.0) + warmup + 30.0
        t_end = time.time() + budget
        script_done_at = 0.0
        prev_img = None
        last_ext_ms = 0.0            # 上一次抓旁证帧的时刻（见 --ext-every-ms）

        def grab_ext(score):
            """抓一张**旁证帧**（客户区 PNG）+ 记一行 metrics.jsonl。

            ⚠ 只在**关键帧**调用（开局 / 判定），别塞进轮询里：抓一张 = 容器侧的合成/编码 +
              跨线程取帧，实测**运行期**连抓会把目标窗口的渲染压停（画面从 ~10s 起完全冻结、
              脚本变成瞎子）——同一份脚本、同一个容器：开着抓 10~60 分，关掉 110 分。
            """
            nonlocal frames, prev_img, last_ext_ms
            last_ext_ms = time.time() * 1000.0
            p = os.path.join(ext_dir, "e%05d.png" % frames)
            with open(p, "wb") as f:
                f.write(conn.screen_png(win.get("id")))
            img = E.read_png(p)
            m = {"i": frames, "tMs": int((time.time() - started) * 1000),
                 "file": os.path.relpath(p, TASK), "w": img[0], "h": img[1],
                 "colors": E.unique_colors(img), "nonblack": round(E.nonblack_ratio(img), 4),
                 "drift": round(E.diff_ratio(prev_img, img), 4) if prev_img else 0.0,
                 "score": score}
            prev_img = img
            with open(metrics_path, "a", encoding="utf-8") as f:
                f.write(json.dumps(m, ensure_ascii=False) + "\n")
            frames += 1
        # 容器侧的日志缓冲会**跨轮累积**（同一个容器一直在用）：先记下当前累计行号当基线，
        # 这一局只看这之后的行（见 read_reports 的 offset）
        try:
            log_from = conn.logs().get("total") or 0
        except Exception:
            log_from = 0
        report = read_reports(conn, log_from)
        log("观察中（最多 %.0fs；通关线 %s 分，看它打印的 score）…" % (budget, target))
        while time.time() < t_end:
            report = read_reports(conn, log_from)
            # 通关判定优先于"被打死"：本体打到目标分时打 "[shooter] CLEAR score=.. target=.."
            # （双保险：也认 score >= target —— 万一那一行没被读到）
            if report["clear"] or (report["score"] is not None and target > 0
                                   and report["score"] >= target):
                verdict, detail = "cleared", "通关：score=%s（目标 %s）" % (report["score"], target)
                break
            if report["over"]:
                verdict, detail = "game_over", "被打死：score=%s" % report["score"]
                break
            try:
                progs = {p.get("name") for p in conn.programs()}
                if program not in progs:
                    verdict, detail = "program_gone", "游戏进程不在了"
                    break
                if E.PLAY_PROGRAM not in progs:
                    verdict, detail = "script_gone", "脚本进程退出了"
                    break
                # ⚠ 旁证帧：默认**只在关键帧**抓（见 --ext-every-ms 的说明）。
                if args.ext_every_ms > 0:
                    if (time.time() * 1000.0 - last_ext_ms) >= args.ext_every_ms:
                        grab_ext(report["score"])
                elif frames == 0:
                    grab_ext(report["score"])           # 0 = 只要关键帧：开局这一张
            except Exception as ex:
                log("抓帧失败: %r" % ex)
                time.sleep(0.5)
            if script_done_at == 0.0 and os.path.exists(session_path) \
                    and os.path.getsize(session_path) > 0:
                script_done_at = time.time() + 8.0     # 脚本收工后再给 8s 宽限（游戏可能还在跑）
                log("脚本已收工，再观察 8s")
            if script_done_at and time.time() > script_done_at:
                verdict, detail = "running", "脚本已收工；游戏还在跑（score=%s）" % report["score"]
                break
            time.sleep(max(0.05, (args.poll_ms or step_ms) / 1000.0))
        else:
            verdict, detail = "running", "到点还在跑（score=%s）" % report["score"]
        # 判定时刻再补一张旁证（打在结论上）；失败不影响判定
        if args.ext_every_ms <= 0:
            try:
                grab_ext(report["score"])
            except Exception as ex:
                log("末尾抓帧失败: %r" % ex)
    finally:
        if own_container and box is not None and not args.keep:
            box.__exit__(None, None, None)
            log("容器已收掉")

    # 中途 break 出来的 cleared/game_over 都算"这局真跑起来了"；整局没打印 = 本体没跑起来
    if verdict == "running" and not report["seen"]:
        verdict, detail = "no_report", "整局没有任何 [shooter] 打印（本体没跑起来？看容器日志: GET /api/logs）"
    passed = bool(report["seen"]) and verdict in ("running", "game_over", "cleared")
    result = {
        "play": cfg["play"], "verdict": verdict, "pass": passed, "detail": detail,
        "report": {"seen": report["seen"], "score": report["score"], "lives": report["lives"],
                   "over": report["over"], "clear": report["clear"], "target": target,
                   "lines": len(report["lines"])},
        "externalFrames": frames,
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
    lines = ["# shooter 一局判定", "",
             "- 结论: **%s**（%s）" % (verdict, detail),
             "- 本体打印: score=%s/%s lives=%s over=%s clear=%s（%d 行）"
             % (report["score"], target, report["lives"], report["over"], report["clear"],
                len(report["lines"])),
             "- 观察 %d 帧 / 用时 %.1fs" % (frames, result["elapsedSec"]),
             "- 容器: 进程 %s，窗口 %s，桌面 %s%s"
             % (program, cfg["window"], cfg["desktop"], "（自起）" if own_container else "（外部给）"),
             ""]
    if best:
        lines += ["关键帧: ![f](%s)" % best, ""]
    with open(os.path.join(out, "report.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines))

    print("== shooter 判定: %s（%s）｜score=%s｜观察 %d 帧｜用时 %.1fs"
          % (verdict, detail, report["score"], frames, result["elapsedSec"]))
    print("PASS" if passed else "FAIL")
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
