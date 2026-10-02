#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
game_play 任务 —— 驱动（**一个配置文件说了算**）

用法:
  python run.py                    # 读本目录的 task.json
  python run.py --config x.json    # 读指定的配置文件（也可 -c）

流程（对应 task.json 的 `run` 段）：

  ① 拉取 virtual_env —— 起容器（常驻，直到本进程收工）
  ② 拉取 agent_cli  —— 起 agent 进程（常驻；工作区 = 本任务目录，它自己读 prompt.md / 改 play.ts）
  ③ 循环（每轮）：
       a) 让 agent 设计/修改脚本 —— POST /task（阻塞一轮；**不给 verify_cmd**，验证由驱动做）
       b) 编译 —— tsc（agent 改的是 .ts，容器装的是 .js）；编译失败就把错误带进分析轮
       c) 跑一局 —— 卸旧程序 → 装 game + 装 play → 外面抓帧 → 等脚本收工
       d) 判分 —— verify.py（子进程）→ 归档 runs/round_NNN
       e) 让 agent 看这一局的证据、自己决定要不要改 —— POST /task
       f) 出口 —— 轮次/时间上限、或"连续 N 轮没进展"（脚本没改 且 判分没变）
  收工：停 agent_cli、收容器。

角色（别混）:
  · `play/play.ts` —— **最终产出**（由 agent 迭代；本文件不碰它）
  · `game/*.ts`    —— **题目侧**玩法（不许改）
  · `verify.py`    —— **尺子**（这里只是"加载"它：当子进程跑，读它的结论）
  · 本文件         —— 驱动 / 编排 / 守夜（保证 agent 一直在跑 + 超时与停滞检测）
"""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
from contextlib import ExitStack

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
BUILD = os.path.join(HERE, "build")
RUNS = os.path.join(HERE, "runs")
CUR = os.path.join(RUNS, "current")
LIVE = os.path.join(CUR, "live.json")     # 运行时现场(给旁路的 watch.py 看: 容器/agent 地址、轮次、阶段)
HUMAN = os.path.join(CUR, "human.json")   # 人类干预记录(watch.py 写, 本驱动读: 下一轮告诉 agent)
sys.path.insert(0, HERE)
import env as E  # noqa: E402

DEFAULT_ROM = os.path.join("E:/work/simulator", "火焰之纹章-圣魔之光石完美修改版.gba")


# ==================== 小工具 ====================
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
    """把文件清空（不存在就建一个空的）。

    ⚠ 不用 os.remove：本机 python 的 shim 会把删除变成"移入回收站"，遇上正被占用的文件
    （容器/agent_cli 的 stdout 日志）会抛 SHFileOperationW 0x20；而这里要的语义本来就是
    "抹掉上一轮的内容"，清空更稳。
    """
    try:
        with open(path, "w", encoding="utf-8"):
            pass
    except Exception:
        pass


def round_index():
    if not os.path.isdir(RUNS):
        return 0
    return len([d for d in os.listdir(RUNS) if d.startswith("round_")])


# ==================== 旁路观测 / 人类干预的口子（见 watch.py） ====================
def publish_live(ctx, phase, round_no=0, box=None, agent=None, extra=None):
    """把"现在跑到哪儿了"写进 runs/current/live.json —— 给旁路的 watch.py（人）看。

    人靠这份文件找到容器与 agent_cli 的地址、知道当前第几轮/什么阶段；每轮与阶段变化都
    覆盖写一次（临时文件 + 原子替换，读的人不会读到半截）。
    ⚠ 里面有 agent 的 http token（本机文件，runs/ 不入库）。它只是"看与插手"的口子，
      不放权：能改什么仍然由 agent 自己那套红线与审核决定。
    """
    j = {
        "ts": int(time.time()), "pid": os.getpid(), "phase": phase, "round": int(round_no),
        "play": ctx.get("play", ""), "app": ctx.get("app", ""), "program": ctx.get("program", ""),
        "window": ctx.get("window", ""), "native": list(ctx.get("native") or []),
        "desktop": ctx.get("desktop", ""), "status_path": ctx.get("status_path", ""),
        "container_url": (box.c.base if box is not None else "") or ctx.get("container_url", ""),
        "task_dir": HERE, "run_dir": CUR,
        "play_ts": ctx.get("play_ts", ""), "prompt_file": ctx.get("prompt_file", ""),
    }
    if agent is not None:
        j["agent_url"] = getattr(agent, "url", "")
        j["agent_token"] = getattr(agent, "token", "")
    if extra:
        j.update(extra)
    try:
        os.makedirs(CUR, exist_ok=True)
        tmp = LIVE + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(j, f, ensure_ascii=False, indent=2)
        os.replace(tmp, LIVE)
    except Exception:
        pass
    return j


def read_human(limit=3):
    """人在外面（watch.py）直接给 agent 下过的任务：取最近 limit 条。

    为什么要读它：agent 自己的会话里没有这段，但文件已经被那些人改过。不讲清楚，
    它下一轮会按"上一轮的认知"动手，跟文件现状打架。
    """
    out = []
    try:
        with open(HUMAN, encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    out.append(json.loads(line))
                except Exception:
                    continue
    except Exception:
        return []
    return out[-limit:]


def human_note():
    """把"人类插过手"写成一段给 agent 看的话（没有就返回空串）。"""
    hs = read_human()
    if not hs:
        return ""
    s = "\n【人类干预】注意：**有人在你之外也直接给这个 agent 下过任务**（他们看着同一块画面）：\n"
    for h in hs:
        s += "  · [%s] %s%s\n" % (h.get("ts_text", "?"), (h.get("prompt") or "(只发了画面)")[:200],
                                 "（附了截图）" if h.get("with_screen") else "")
    s += "  以**文件现状**为准：动手前先看 play/play.ts 现在是什么样，别按你上一轮的印象改。\n"
    return s


def abspath(path):
    """配置里的相对路径按本目录解析（本文件在任务目录下）。"""
    if not path:
        return ""
    return path if os.path.isabs(path) else os.path.normpath(os.path.join(HERE, path))


def read_text(path, limit=200000):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read(limit)
    except Exception:
        return ""


def file_sig(path):
    """文件指纹（判"agent 这一轮到底改了没有"）。"""
    if not os.path.exists(path):
        return "(missing)"
    h = hashlib.sha1()
    with open(path, "rb") as f:
        h.update(f.read())
    return h.hexdigest()[:12]


def ensure_agent_config(bin_path, config_path, root_dir):
    """agent_cli 的 config.json：没有就让它自己 --init 生成一份，然后统一改写——
      · http.port 换成空闲端口（8080 可能被占/多实例撞车）
      · terminal.rootDir = 本任务目录（agent 要在这里读 prompt.md、改 play/play.ts）
    """
    if not config_path:
        return ""
    if not os.path.exists(config_path):
        os.makedirs(os.path.dirname(config_path), exist_ok=True)
        run([bin_path, "--init", "-c", config_path], check=False)
    cfg = E.read_json(config_path, {}) or {}
    if not isinstance(cfg, dict):
        return config_path
    http = cfg.get("http") if isinstance(cfg.get("http"), dict) else None
    if http is not None:
        http["port"] = E.free_port()
    term = cfg.get("terminal") if isinstance(cfg.get("terminal"), dict) else None
    if term is not None:
        term["rootDir"] = root_dir
    with open(config_path, "w", encoding="utf-8") as f:
        json.dump(cfg, f, ensure_ascii=False, indent=2)
    return config_path


# ==================== 给 agent 的两段任务（"生成"与"分析"） ====================
def env_brief(ctx, agent_url=""):
    return (
        "【环境】\n"
        "· 虚拟设备容器: %s（无头虚拟桌面；玩法 %s：进程名 %s、窗口标题 %s、客户区 %dx%d）\n"
        "· 任务书: %s（**先读它**：红线、产出物形状、观测面都在里面）\n"
        "· 产出物（你要改的就是它）: %s\n"
        "· 证据目录: %s（每轮被覆盖：play.log / frames/ / external/ / metrics.jsonl / report.md / session.json / drive.json / result.json）\n"
        "· 你的工作区: %s\n"
        "· 驱动会做: 编译你的脚本 → 在容器里跑一局 → 跑尺子 verify.py → 归档；你不用自己跑实验。\n"
        % (ctx["container_url"], ctx["play"], ctx["program"], ctx["window"], ctx["native"][0],
           ctx["native"][1], ctx["prompt_file"], ctx["play_ts"], CUR, HERE)
    )


def design_prompt(ctx, taskbook, round_no, last_summary):
    return (
        env_brief(ctx) +
        "\n【任务】第 %d 轮：为这个游戏设计（或**改进**）一键通关脚本。\n"
        "%s"
        "把脚本写到 %s 就停手 —— 驱动会编译并跑一局，下一轮你会拿到这一局的证据。\n"
        "⚠ 只看画面/只按键（红线见任务书）；不要改 game/**、verify.py、task.json。\n"
        "\n【任务书原文】\n%s\n"
        % (round_no, last_summary, ctx["play_ts"], taskbook)
    )


def analyze_prompt(ctx, round_no, verify_rc, verify_out, build_out, archive_dir, human=""):
    extra = human          # "人类插过手"的那段话（watch.py 写下的，见 human_note()）
    if build_out:
        extra += "⚠ 这一轮**编译没过**（脚本有语法/类型错，先修它），tsc 输出：\n" + build_out[-4000:] + "\n"
    return (
        env_brief(ctx) +
        "\n【任务】第 %d 轮已跑完，请**自己看证据**判断：这一局正常吗？脚本该怎么改？\n"
        "· 证据目录: %s（play.log 是脚本每拍一行；report.md 是摘要＋关键帧；metrics.jsonl 是外部帧的差异）\n"
        "· 上一次归档: %s\n"
        "· 尺子退出码: %s\n"
        "· 尺子输出（末尾）：\n%s\n"
        "%s"
        "要改就直接改 %s（用 modify_file/read_file 等工具）；这轮**不要**跑实验（驱动会跑）。\n"
        "如果这一版你已经认为可以了，就在回复里写一句 `[script-ok]`。\n"
        % (round_no, CUR, archive_dir, verify_rc, (verify_out or "")[-4000:], extra, ctx["play_ts"])
    )


# ==================== 一局（交给**玩法自己的尺子**跑 + 判） ====================
def container_port(box):
    """容器 HTTP 端口（从 base url 里取；给玩法尺子用）。"""
    return int(str(box.c.base).rsplit(":", 1)[-1])


def one_round(box, ctx, round_no):
    """跑一局 + 判分 —— 交给玩法自己的 `verify.py`：

    它自己清掉容器里的程序、装 game + play、从外面抓帧、读状态（score/lives/over/win），
    给出 verdict（cleared / game_over / running / …）。驱动只负责把它跑起来、拿退出码、归档。
    """
    verify_path = abspath(ctx["verify"])
    if not os.path.exists(verify_path):
        print("FAIL: 找不到玩法自己的尺子: %s" % verify_path)
        return {"rc": 1, "out": "", "pass": False, "archive": ""}

    args = [sys.executable, verify_path, "--play", ctx["play"],
            "--port", str(container_port(box)), "--out", CUR,
            "--game-js", ctx["game_js"], "--play-js", ctx["play_js"],
            "--steps", str(ctx["steps"]), "--step-ms", str(ctx["step_ms"]),
            "--warmup", str(ctx["warmup"])]
    if ctx["seconds"] > 0:
        args += ["--seconds", str(ctx["seconds"])]
    if ctx.get("target"):
        args += ["--target", str(ctx["target"])]
    rc, out = run(args, cwd=HERE, check=False)
    passed = ("PASS" in out.splitlines()) or (rc == 0)

    # ---- 归档 ----
    archive = ""
    if ctx["archive"]:
        archive = os.path.join(RUNS, "round_%03d" % (round_index() + 1))
        os.makedirs(archive, exist_ok=True)
        for name in ("session.json", "metrics.jsonl", "result.json", "report.md", "play.log",
                     "play_stdout.txt", "container_stdout.txt", "agent_cli_stdout.txt",
                     "human.json"):     # 人插过的手也是证据（watch.py 写的，见该文件头）
            src = os.path.join(CUR, name)
            if os.path.exists(src):
                shutil.copy2(src, os.path.join(archive, name))
        for sub in ("frames", "external"):
            dirpath = os.path.join(CUR, sub)
            files = sorted(f for f in os.listdir(dirpath) if f.endswith(".png")) \
                if os.path.isdir(dirpath) else []
            keep = files[::5] + ([files[-1]] if files and files[-1] not in files[::5] else [])
            os.makedirs(os.path.join(archive, sub), exist_ok=True)
            for f in keep:
                shutil.copy2(os.path.join(dirpath, f), os.path.join(archive, sub, f))
        with open(os.path.join(RUNS, "scores.jsonl"), "a", encoding="utf-8") as f:
            f.write(json.dumps({"round": os.path.basename(archive), "play": ctx["play"],
                                "pass": passed, "ts": int(time.time())}, ensure_ascii=False) + "\n")
        print("== 归档 -> %s" % os.path.relpath(archive, ROOT))
    return {"rc": rc, "out": out, "pass": passed, "archive": archive}


# ==================== 主流程 ====================
def main():
    ap = argparse.ArgumentParser(description="game_play 驱动：一个配置文件说了算（见文件头注释）")
    ap.add_argument("--config", "-c", default="", help="配置文件（默认：本目录 task.json）")
    args = ap.parse_args()

    # 旁路观测/干预的口子（见 watch.py）：每次跑局开新的 —— 先把上一轮的现场与人类指令清掉，
    # 否则 watch.py 会照着上一轮的地址去连（那块容器早没了）。
    os.makedirs(CUR, exist_ok=True)
    for p in (LIVE, HUMAN):
        try:
            os.remove(p)
        except FileNotFoundError:
            pass
        except Exception:
            fresh_file(p)   # 本机 python 的 os.remove 可能变成"移入回收站"而失败；清空一样能用

    cfg_path = args.config or os.path.join(HERE, "task.json")
    cfg = E.read_json(cfg_path, None)
    if not isinstance(cfg, dict):
        print("FAIL: 读不到配置文件、或它不是 JSON 对象: %s" % cfg_path)
        return 1
    run_cfg = cfg.get("run") or {}
    print("== 配置文件: %s" % os.path.relpath(cfg_path, ROOT))

    # ---- 玩法 + 本轮参数 ----
    try:
        play = E.play_config(run_cfg.get("play") or "", cfg)
    except Exception as ex:
        print("FAIL: %s" % ex)
        return 1
    cfgname = run_cfg.get("cfg", "Debug")
    session_cfg = cfg.get("session") or {}
    ctx = {
        "play": play["play"], "app": play["app"], "program": play["program"],
        "window": play["window"], "need_rom": bool(play.get("rom")),
        "desktop": run_cfg.get("desktop") or play["desktop"],
        "native": [int(play["native"][0]), int(play["native"][1])],
        # 节奏：**本次跑法(run 段) > 玩法自己的配置 > 任务的 session 段 > 硬编码默认**
        #   玩法目录 config.json 里写了 steps/stepMs/warmupSec 就按它来；run 段显式写了
        #   才是临时覆盖（调参用）。
        "steps": int(run_cfg.get("steps") or play.get("steps") or session_cfg.get("steps") or 120),
        "step_ms": int(run_cfg.get("stepMs") or play.get("stepMs") or session_cfg.get("stepMs") or 400),
        "warmup": float(run_cfg.get("warmupSec") or play.get("warmupSec")
                        or session_cfg.get("warmupSec") or 3.0),
        "seconds": float(run_cfg.get("seconds", session_cfg.get("seconds", 0.0))),
        "timeout": float(run_cfg.get("timeoutSec", 0.0)),
        "archive": bool(run_cfg.get("archive", True)),
        # 尺子 = **玩法自己的**（玩法目录 config.json 里的 verify）；没人给才回落到驱动的 run.verify
        "verify": play.get("verify") or run_cfg.get("verify") or "",
        "target": int(play.get("targetScore", 0) or 0),
        "play_ts": os.path.join(HERE, "play", "play.ts"),
        # 玩法的状态文件（玩法自己 config.json 里声明 statusPath）—— watch.py 靠它把"现在几分"打给人看
        "status_path": play.get("statusPath") or "",
        "prompt_file": abspath(run_cfg.get("prompt") or cfg.get("prompt") or "prompt.md"),
        "rom": run_cfg.get("rom") or cfg.get("rom") or DEFAULT_ROM,
        "container_url": "",
    }
    agent_cfg = run_cfg.get("agent") or {}
    rounds_cfg = run_cfg.get("rounds") or {}
    max_rounds = int(rounds_cfg.get("max", cfg.get("rounds", {}).get("max", 50)))
    round_timeout = float(rounds_cfg.get("timeoutSec", 7200))
    stall_rounds = int(rounds_cfg.get("stallRounds", 3))
    retry_sec = float(rounds_cfg.get("retrySec", cfg.get("rounds", {}).get("retrySec", 5)))
    max_minutes = float(run_cfg.get("maxMinutes", 0))          # 0 = 不限
    deadline = time.time() + max_minutes * 60 if max_minutes > 0 else None
    stop_when = run_cfg.get("stopWhen", "rounds")      # rounds | verify-pass
    want_build = bool(run_cfg.get("build", True))

    # ---- 依赖 ----
    tsc = E.tsc_exe(cfgname)
    exe = E.container_exe(cfgname)
    dll_dir = run_cfg.get("dllDir") or E.emulator_dll_dir(cfgname)
    need = [(exe, "容器 virtual_device.exe"), (tsc, "tsc")]
    if ctx["need_rom"]:
        need += [(os.path.join(dll_dir, "mgba_host.dll"), "mgba_host.dll"), (ctx["rom"], "ROM")]
    for p, why in need:
        if not os.path.exists(p):
            print("FAIL: 找不到 %s: %s" % (why, p))
            return 1
    print("== 玩法: %s（%s）进程名=%s 窗口=%s 桌面=%s 原生=%dx%d%s"
          % (ctx["play"], ctx["app"], ctx["program"], ctx["window"], ctx["desktop"],
             ctx["native"][0], ctx["native"][1], "（要 ROM）" if ctx["need_rom"] else ""))
    # 玩法本体的 JS：默认在任务 build/ 下（js 是相对 build 的路径）；玩法配了 jsRoot="repo"
    # 就是"本体住在别处"（如 shooter 直接用 binary/code_agent/runtime/test_jsshooter 的产物）
    ctx["game_js"] = E.play_js_path(play, HERE, BUILD)
    ctx["play_js"] = os.path.join(BUILD, "play", "play.js")
    taskbook = read_text(ctx["prompt_file"])

    last_verify = None
    agent_factory = None
    with ExitStack() as stack:
        # ---- ① 拉取 virtual_env ----
        box = stack.enter_context(
            E.HeadlessContainer(cfg=cfgname, desktop=ctx["desktop"], exe=exe,
                                log_path=os.path.join(CUR, "container_stdout.txt")))
        ctx["container_url"] = box.c.base
        publish_live(ctx, "container-ready", 0, box)   # 告诉 watch.py: 容器在这儿

        # ---- ② 拉取 agent_cli ----
        agent = None
        if agent_cfg.get("enabled", True):
            bin_path = abspath(agent_cfg.get("bin") or ((cfg.get("agent") or {}).get("bin") or ""))
            if not bin_path or not os.path.exists(bin_path):
                msg = "找不到 agent_cli: %s（配 run.agent.bin 或顶层 agent.bin）" % (bin_path or "(空)")
                if agent_cfg.get("required", True):
                    print("FAIL: " + msg)
                    return 1
                print("!! " + msg)
            else:
                cli_cfg = ensure_agent_config(bin_path, abspath(agent_cfg.get("config", "")), HERE)

                def make_agent():
                    return E.AgentCliProc(bin_path, config_path=cli_cfg,
                                          http_url=agent_cfg.get("httpUrl", ""),
                                          token=str(agent_cfg.get("token", "")),
                                          ready_sec=int(agent_cfg.get("readySec", 120)))

                agent_factory = make_agent
                agent = agent_factory()
                stack.callback(agent.stop)
                if not agent.start(log_path=os.path.join(CUR, "agent_cli_stdout.txt")):
                    if agent_cfg.get("required", True):
                        print("FAIL: agent_cli 没起来（run.agent.required=true）")
                        return 1
                    print("!! agent_cli 没起来 —— 只跑局，不驱动 agent")
                    agent = None
                else:
                    print("== agent_cli READY: %s（工作区=%s）" % (agent.url, HERE))
                    publish_live(ctx, "agent-ready", 0, box, agent)   # 告诉 watch.py: agent 在这儿
        else:
            print("!! run.agent.enabled=false：不驱动 agent（只跑局）")
            os.makedirs(CUR, exist_ok=True)

        # ---- ③ 循环 ----
        stall, r, last_sig = 0, 0, None
        while r < max_rounds and (deadline is None or time.time() < deadline):
            r += 1
            print("\n================ 第 %d 轮 ================" % r)
            publish_live(ctx, "design", r, box, agent)   # 告诉 watch.py: 这一轮开始了

            # a) 生成/修改脚本
            if agent:
                summary = ""
                if last_verify:
                    summary = ("上一轮尺子判分: %s；上一次归档: %s\n"
                               % ("PASS" if last_verify[1] else "FAIL", last_verify[2] or "(无)"))
                # 人插过的手（watch.py 直接给这个 agent 下过任务）也带进这一轮 ——
                # 否则它按"上一轮的认知"动手，跟被别人改过的文件现状打架。
                hn = human_note()
                if hn:
                    print("== 检测到人类干预（见 runs/current/human.json），已带进这一轮上下文")
                summary += hn
                try:
                    resp = agent.submit_task(design_prompt(ctx, taskbook, r, summary),
                                             timeout=round_timeout)
                    print("== agent(设计) 回包: status=%s error=%s reply=%dB"
                          % (resp.get("status"), resp.get("error") or "-",
                             len(resp.get("agent_reply") or "")))
                except Exception as ex:
                    print("!! 提交任务失败: %r" % ex)
                    if agent and not agent.alive() and agent_factory and agent_cfg.get("restart", True):
                        print("== agent_cli 不在了，重拉…")
                        agent = agent_factory()
                        stack.callback(agent.stop)
                        if not agent.start(log_path=os.path.join(CUR, "agent_cli_stdout.txt")):
                            print("!! agent_cli 重拉失败 —— 后面只跑局")
                    time.sleep(retry_sec)

            # b) 编译（agent 改的是 .ts；编译不过就把错误交给分析轮）
            build_out = ""
            build_ok = True
            if want_build:
                rc, build_out = run([tsc, "-p", os.path.join(HERE, "tsconfig.json")],
                                    cwd=HERE, check=False)
                # 玩法声明的"别的工程"（本体住在别处，如 shooter 直接复用 test_jsshooter）
                for proj in E.play_prebuild(play):
                    rc2, out2 = run([tsc, "-p", proj], cwd=os.path.dirname(proj), check=False)
                    build_out += out2
                    if rc2 != 0:
                        rc = rc2
                build_ok = (rc == 0) and os.path.exists(ctx["play_js"]) \
                    and os.path.exists(ctx["game_js"])
                if not build_ok:
                    print("!! 编译失败（或产物缺失）—— 跳过跑局，把错误交给 agent 修")

            # c) 跑一局 + d) 判分 + 归档
            res = one_round(box, ctx, r) if build_ok else \
                {"rc": 1, "out": "（编译没过，本轮没跑局）", "pass": False, "archive": ""}
            last_verify = (r, res["pass"], res["archive"])

            # e) 分析轮（agent 自己看证据、自己决定改不改）
            reply = ""
            if agent:
                try:
                    resp = agent.submit_task(
                        analyze_prompt(ctx, r, res["rc"], res["out"], "" if build_ok else build_out,
                                       res["archive"], human_note()), timeout=round_timeout)
                    reply = resp.get("agent_reply") or ""
                    print("== agent(分析) 回包: status=%s error=%s reply=%dB"
                          % (resp.get("status"), resp.get("error") or "-", len(reply)))
                except Exception as ex:
                    print("!! 分析轮提交失败: %r" % ex)

            # f) 出口
            if stop_when == "verify-pass" and res["pass"]:
                print("== 尺子 PASS，按 run.stopWhen=verify-pass 收工")
                break
            sig = (file_sig(ctx["play_ts"]), res["rc"])
            if sig == last_sig:
                stall += 1
                print("== 没进展（脚本没改 且 判分没变）: %d/%d" % (stall, stall_rounds))
                if stall >= stall_rounds:
                    print("== 连续 %d 轮没进展，收工" % stall)
                    break
            else:
                stall = 0
                last_sig = sig
            if agent and "[script-ok]" in (reply or ""):
                print("== agent 说这一版可以了（[script-ok]），收工")
                break

        publish_live(ctx, "stopped", r, box, agent)   # 告诉 watch.py: 驱动收工了
        print("\n== 收工: 跑了 %d 轮；最后一次判分 %s；最终脚本 %s"
              % (r, "PASS" if (last_verify and last_verify[1]) else "FAIL", ctx["play_ts"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
