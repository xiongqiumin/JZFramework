#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
game_play 任务 —— **旁路观察 / 中途插手**（人在环；不参与 run.py 的主循环）

run.py 是"机器自己跑"的驱动；本文件是**你**（人）的窗口和手柄 —— 它只连两个现成的口子：
容器的 HTTP（看画面、数进程）与 agent_cli 的 HTTP（看它在忙什么、打断、下任务），
**不改代码、不动驱动**。

看：
  python watch.py                          # 看一眼：第几轮/什么阶段、画面、agent、玩法的分、日志尾
  python watch.py --watch                  # 连续看（默认 2s 一次；Ctrl+C 退出）
  python watch.py --watch --interval 5     # 慢一点
  python watch.py --shot                   # 只抓一张画面，存 runs/current/watch/shot_HHMMSS.png
  python watch.py --no-shot                # 只看状态，不抓画面（省事）
插手：
  python watch.py --interrupt              # 打断 agent_cli **当前**正在跑的任务（POST /reset scope=task）
  python watch.py --ask "把脚本改成先找红球" # 给 agent 下一道**你的**活（POST /task）
  python watch.py --ask "看这张图：球在左上角" --with-screen   # 把你看到的画面一起发过去（多模态）
  python watch.py --interrupt --ask "..."  # 接管：先打断它手上的活，再让它做我的
  python watch.py --ask "..." --wait       # 等它做完再返回（默认提交完就返回）

怎么找到"正在跑的那套东西"：
  run.py 每轮把现场写进 `runs/current/live.json`（容器/agent 的地址、玩法、窗口、轮次、阶段），
  本文件读它。没有它（=run.py 没在跑）时，用 --container-url / --agent-url 手动指。

⚠ 三条边界（与任务书同一套红线）：
  · 只**看画面**、只**打字**：不读模拟器内存、不碰 verify.py 与 game/**；
  · 你下的任务与 run.py 的任务**共用同一个 agent_cli**（忙时会 409）—— 要接管先 --interrupt；
  · 你插手的记录会写进 `runs/current/human.json`，run.py 下一轮把它带进 agent 的上下文
    （否则 agent 不知道文件被"别人"改过，会按上一轮的印象动手）。
"""
import argparse
import base64
import hashlib
import json
import os
import sys
import time
import urllib.error
import urllib.request

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

HERE = os.path.dirname(os.path.abspath(__file__))
RUNS = os.path.join(HERE, "runs")
CUR = os.path.join(RUNS, "current")
LIVE = os.path.join(CUR, "live.json")
HUMAN = os.path.join(CUR, "human.json")
WATCHDIR = os.path.join(CUR, "watch")

sys.path.insert(0, HERE)
import env as E  # noqa: E402


# ==================== 读/发 ====================
def read_live():
    d = E.read_json(LIVE, None)
    return d if isinstance(d, dict) else {}


def read_text(path, limit=400000):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read(limit)
    except Exception:
        return ""


def tail(path, n=5):
    return [l for l in read_text(path).splitlines() if l.strip()][-n:]


def http_json(base, path, body=None, token="", timeout=10):
    """对 agent_cli 发 JSON 请求 → (http_code, dict)。

    4xx/5xx 也把响应体解析回来（409 的 "a task is already running"、401 的鉴权错都在体里）。
    连不上返回 (0, {"error": ...})。
    """
    data = None if body is None else json.dumps(body, ensure_ascii=False).encode("utf-8")
    req = urllib.request.Request(base.rstrip("/") + path, data=data,
                                 method="POST" if data is not None else "GET")
    if data is not None:
        req.add_header("Content-Type", "application/json")
    if token:
        req.add_header("Authorization", "Bearer " + token)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, json.loads((r.read() or b"{}").decode("utf-8", "replace"))
    except urllib.error.HTTPError as ex:
        raw = ex.read() or b"{}"
        try:
            return ex.code, json.loads(raw.decode("utf-8", "replace"))
        except Exception:
            return ex.code, {"error": "HTTP %d: %s" % (ex.code, raw[:200])}
    except Exception as ex:
        return 0, {"error": "%r" % (ex,)}


def place(args):
    """定下"看哪儿/发给谁"：live.json 优先，命令行参数可覆盖（完全没有 live.json 时全靠它）。"""
    live = read_live()
    return {
        "live": live,
        "container_url": args.container_url or live.get("container_url") or "",
        "agent_url": args.agent_url or live.get("agent_url") or "",
        "token": args.token or live.get("agent_token") or "",
        "window": args.window or live.get("window") or "",
        "play": live.get("play") or "",
        "status_path": live.get("status_path") or "",
        "round": live.get("round") or 0,
        "phase": live.get("phase") or "(未知)",
    }


# ==================== 画面 ====================
def container_of(pl):
    url = pl["container_url"]
    if not url:
        return None, "没有容器地址（run.py 没在跑？或用 --container-url 指一块）"
    return E.Container(int(str(url).rsplit(":", 1)[-1])), ""


def find_window(c, title):
    """按标题找窗口（找不到就 None -> 抓整块桌面）。"""
    if not title:
        return None
    try:
        for w in c.windows():
            if str(w.get("title") or "").upper() == str(title).upper():
                return w
    except Exception:
        return None
    return None


def grab(pl, keep=False):
    """抓一张画面存到 runs/current/watch/。返回 (路径, 错误)。"""
    c, err = container_of(pl)
    if err:
        return "", err
    try:
        win = find_window(c, pl["window"])
        png = c.screen_png(win.get("id") if win else None)
    except Exception as ex:
        return "", "抓帧失败: %r" % (ex,)
    os.makedirs(WATCHDIR, exist_ok=True)
    path = os.path.join(WATCHDIR, ("shot_%s.png" % time.strftime("%H%M%S")) if keep else "live.png")
    try:
        with open(path, "wb") as f:
            f.write(png)
    except Exception as ex:
        return "", "存盘失败: %r" % (ex,)
    return path, ""


# ==================== 看一眼 ====================
def show(pl, args):
    live = pl["live"]
    print("== game_play 旁路观测  %s" % time.strftime("%Y-%m-%d %H:%M:%S"))
    if not live:
        print("!! 没读到 %s —— run.py 大概没在跑（也可以 --container-url/--agent-url 手动指）"
              % os.path.relpath(LIVE, HERE))
    else:
        nat = live.get("native") or ["?", "?"]
        print("· 驱动     pid=%s  第 %s 轮  阶段=%s" % (live.get("pid", "?"), pl["round"], pl["phase"]))
        print("· 玩法     %s（进程名 %s，窗口 %s，原生 %sx%s，桌面 %s）"
              % (pl["play"] or "?", live.get("program") or "?", pl["window"] or "?",
                 nat[0], nat[1], live.get("desktop") or "?"))

    # 脚本指纹：谁改了 play.ts（agent 还是人）
    ts = live.get("play_ts") or os.path.join(HERE, "play", "play.ts")
    if os.path.exists(ts):
        with open(ts, "rb") as f:
            sig = hashlib.sha1(f.read()).hexdigest()[:12]
        print("· 脚本     %s  指纹=%s  %dB  改于 %s"
              % (os.path.relpath(ts, HERE), sig, os.path.getsize(ts),
                 time.strftime("%H:%M:%S", time.localtime(os.path.getmtime(ts)))))

    # 容器里的进程（证明"容器活着、程序在跑"）
    c, err = container_of(pl)
    if err:
        print("· 容器     (没有) %s" % err)
    else:
        try:
            names = ", ".join((p.get("name") or "?") for p in c.programs()) or "(空)"
            print("· 容器     %s  程序=%s" % (pl["container_url"], names))
        except Exception as ex:
            print("· 容器     %s  读程序表失败: %r" % (pl["container_url"], ex))

    # agent 在忙什么
    if pl["agent_url"]:
        code, d = http_json(pl["agent_url"], "/info", token=pl["token"], timeout=5)
        if code == 200:
            mcp = d.get("mcp") or []
            mcp_txt = ("%d 个(就绪 %d)" % (len(mcp), sum(1 for m in mcp if m.get("ready")))) if mcp else "无"
            print("· agent    %s  busy=%s  round=%s  mcpInitDone=%s  mcp=%s"
                  % (pl["agent_url"], d.get("busy"), d.get("round"), d.get("mcpInitDone"), mcp_txt))
        else:
            print("· agent    %s  读 /info 失败: code=%s %s" % (pl["agent_url"], code, d.get("error")))
    else:
        print("· agent    (没有)")

    # 玩法的分（状态文件）—— 人的眼睛最先看这个
    if pl["status_path"]:
        sp = pl["status_path"]
        p = sp if os.path.isabs(sp) else os.path.join(HERE, sp)
        st = E.read_json(p, None)
        if isinstance(st, dict):
            keep = {k: st[k] for k in ("score", "target", "over", "win", "hits", "misses") if k in st}
            print("· 分数     %s -> %s" % (os.path.relpath(p, HERE), json.dumps(keep, ensure_ascii=False)))
        else:
            print("· 分数     %s -> (还没有)" % os.path.relpath(p, HERE))

    # 判分历史
    lines = tail(os.path.join(RUNS, "scores.jsonl"), 4)
    if lines:
        print("· 判分     scores.jsonl 尾部:")
        for l in lines:
            try:
                o = json.loads(l)
                print("             %s  %s" % (o.get("round"), "PASS" if o.get("pass") else "FAIL"))
            except Exception:
                print("             " + l[:120])

    # 人类干预记录
    hs = [l for l in read_text(HUMAN).splitlines() if l.strip()]
    if hs:
        print("· 人类干预 %d 条，最近:" % len(hs))
        for l in hs[-2:]:
            try:
                o = json.loads(l)
                print("             [%s] %s" % (o.get("ts_text"), (o.get("prompt") or "")[:100]))
            except Exception:
                print("             " + l[:120])

    # 日志尾
    for name in ("play.log", "container_stdout.txt"):
        ls = tail(os.path.join(CUR, name), 4)
        if ls:
            print("· %-18s 尾部:" % name)
            for l in ls:
                print("             " + l[:150])

    # 画面
    if not args.no_shot:
        path, err2 = grab(pl, keep=args.keep)
        print(("· 画面     %s" % os.path.relpath(path, HERE)) if path else ("· 画面     (抓不到) %s" % err2))
    print()


# ==================== 插手 ====================
def do_interrupt(pl):
    """打断 agent_cli 当前任务：POST /reset {"scope":"task"}。

    ⚠ 它同时会清掉 agent 的会话历史（scope=task 走的就是 resetSession），
    所以 run.py 那边正在等的那一轮会以 status=cancelled 收尾 —— 这是**预期**的接管代价。
    """
    if not pl["agent_url"]:
        print("!! 没有 agent 地址 —— 打断谁？（run.py 没在跑时用 --agent-url）")
        return False
    code, d = http_json(pl["agent_url"], "/reset", {"scope": "task"}, token=pl["token"], timeout=30)
    if code == 200:
        print("== 已打断 agent_cli 当前任务: cancelled=%s（它手上那一轮会以 status=cancelled 收尾）"
              % d.get("cancelled"))
        return True
    print("!! 打断失败: code=%s %s" % (code, d.get("error") or d))
    return False


def record_human(args, prompt, images, shot_path, code, resp):
    """记一笔"人插了手"进 runs/current/human.json（run.py 下一轮会读它、告诉 agent）。"""
    rec = {
        "ts": int(time.time()), "ts_text": time.strftime("%Y-%m-%d %H:%M:%S"), "by": "watch.py",
        "prompt": prompt, "with_screen": bool(images),
        "screen": os.path.relpath(shot_path, HERE) if shot_path else "",
        "interrupted": bool(args.interrupt), "http_code": code,
        "status": (resp or {}).get("status") or (resp or {}).get("error", ""),
    }
    try:
        os.makedirs(CUR, exist_ok=True)
        with open(HUMAN, "a", encoding="utf-8") as f:
            f.write(json.dumps(rec, ensure_ascii=False) + "\n")
    except Exception:
        pass


def do_ask(pl, args):
    """给 agent 下一道**人的**任务（可带当前画面）。返回 shell 退出码。"""
    if not pl["agent_url"]:
        print("!! 没有 agent 地址 —— 没人接这道活（run.py 没在跑时用 --agent-url）")
        return 2
    # 任务文字：--ask 直接给，或 --ask-file 从 UTF-8 文件读（中文/长指令更稳：
    # 有些终端把命令行里的中文传成乱码 —— 那是终端的锅，但乱码指令发给 agent 是灾难）
    prompt = args.ask
    if args.ask_file:
        try:
            with open(args.ask_file, encoding="utf-8") as f:
                prompt = f.read().strip()
        except Exception as ex:
            print("!! 读不了 --ask-file %s: %r" % (args.ask_file, ex))
            return 2
    if not prompt.strip() and not args.with_screen:
        print("!! --ask/--ask-file 是空的（只给 --with-screen 也行：让它自己看画面说点什么）")
        return 2

    images, shot_path = [], ""
    if args.with_screen:
        shot_path, err = grab(pl, keep=True)
        if not shot_path:
            print("!! 想带画面但抓不到: %s" % err)
            return 2
        with open(shot_path, "rb") as f:
            images.append("data:image/png;base64," + base64.b64encode(f.read()).decode("ascii"))

    if args.interrupt:
        do_interrupt(pl)

    body = {"prompt": prompt}
    if images:
        body["images"] = images
    print("== 提交人类任务: %d 字%s%s"
          % (len(prompt), ("，附 %d 张画面(%s)" % (len(images), os.path.relpath(shot_path, HERE)))
             if images else "",
             ("（等回包，最多 %gs）" % args.timeout) if args.wait else "（不等回包）"))
    t0 = time.time()
    code, d = http_json(pl["agent_url"], "/task", body, token=pl["token"],
                        timeout=(args.timeout if args.wait else 10))
    record_human(args, prompt, images, shot_path, code, d)

    if code == 200:
        print("== 任务结束: status=%s  用时=%.1fs  回复=%dB"
              % (d.get("status"), time.time() - t0, len(d.get("agent_reply") or "")))
        rep = (d.get("agent_reply") or "").strip()
        if rep:
            print("---- agent 回复(尾 2000 字) ----")
            print(rep[-2000:])
        if d.get("error"):
            print("!! error: %s" % d.get("error"))
        return 0
    if code == 409:
        print("!! agent 正忙（run.py 这一轮的任务在跑）。要接管就加 --interrupt：先打断再下你的活。")
        return 1
    if code == 0 and not args.wait:
        print("== 没等到回包（这是 --wait 缺省的预期行为）—— 任务已经在跑了，用 `python watch.py --watch` 看进展")
        return 0
    print("!! 提交失败: code=%s %s" % (code, d.get("error") or d))
    return 1


# ==================== main ====================
def main():
    ap = argparse.ArgumentParser(description="game_play 旁路观察/干预（人在环；见文件头）")
    ap.add_argument("--watch", action="store_true", help="连续看（默认 2s 一次；Ctrl+C 退出）")
    ap.add_argument("--interval", type=float, default=2.0, help="--watch 的间隔秒数")
    ap.add_argument("--shot", action="store_true", help="只抓一张画面就退出")
    ap.add_argument("--keep", action="store_true", help="画面另存带时间戳的（默认覆盖 watch/live.png）")
    ap.add_argument("--no-shot", action="store_true", help="只看状态，不抓画面")
    ap.add_argument("--ask", default="", help="给 agent 下一道你的任务（POST /task）")
    ap.add_argument("--ask-file", default="", help="从 UTF-8 文本文件读任务（中文/长指令更稳；"
                                                   "某些终端把命令行里的中文传成乱码，用这个绕开）")
    ap.add_argument("--with-screen", action="store_true", help="--ask 时把当前画面一起发过去（多模态）")
    ap.add_argument("--interrupt", action="store_true", help="打断 agent_cli 当前正在跑的任务（POST /reset）")
    ap.add_argument("--wait", action="store_true", help="--ask 时等任务跑完再返回（默认不等）")
    ap.add_argument("--timeout", type=float, default=1800.0, help="--wait 的等待上限秒数")
    ap.add_argument("--container-url", default="", help="容器地址（没有 live.json 时用）")
    ap.add_argument("--agent-url", default="", help="agent_cli 地址（没有 live.json 时用）")
    ap.add_argument("--token", default="", help="agent_cli 的 http token（若配了鉴权）")
    ap.add_argument("--window", default="", help="窗口标题（默认取 live.json 的）")
    args = ap.parse_args()

    pl = place(args)

    if args.ask or args.ask_file or args.with_screen:
        return do_ask(pl, args)
    if args.interrupt:
        return 0 if do_interrupt(pl) else 1
    if args.shot:
        path, err = grab(pl, keep=True)
        print(("== 画面 -> %s" % os.path.relpath(path, HERE)) if path else ("!! 抓不到: %s" % err))
        return 0 if path else 1

    if args.watch:
        try:
            while True:
                show(pl, args)
                pl = place(args)            # 每轮重读 live.json（轮次/阶段会变）
                time.sleep(max(0.2, args.interval))
        except KeyboardInterrupt:
            print("\n== 退出（容器与 agent_cli 都不动 —— 这里只是个观察者）")
            return 0

    show(pl, args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
