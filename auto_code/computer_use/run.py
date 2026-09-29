#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
computer_use 脚手架 —— 一条命令跑一遍（编译 → 出题 → 截图 → 探索 → 判分 → 归档）

用法（本目录下）:
  python run.py                      # 用 build/scenes/gen.json 里已有的种子（默认 0）
  python run.py --seed 7 --level 1   # 换题目
  python run.py --skip-build         # TS 没改，跳过 tsc
  python run.py --no-shot            # 只出题/判分（不拉常驻假 App、不截图）
  python run.py --cfg Release        # 用另一套构建配置（默认 Debug；⚠Release 无 ts_compiler）

流程要点:
  - 出题 = `scenes/gen.ts`（一份声明 → uiLoader 真控件树 + 同源 truth.json）；
  - 屏幕 = 把场景进程**常驻拉起**（`--debug --ui-mcp-port N`，窗口一出现才起 UI MCP），
    用 `mcp.py` 的 `ui_screenshot` 取 PNG 存 `runs/current/shot.png`（截图 840x640 = 2 倍缩放，
    窗口逻辑尺寸 420x320 由 `ui_windows` 的 bounds 给出）—— 探针只看像素，不看窗口内部；
  - 探索 = `probe/explore.ts`（固定入口）读 PNG + 窗口几何 → 注入离线后端 → `App.explore()`；
  - 判分 = `verify.py`（五项硬门），失败打逐节点 diff。

为什么不参与构建（见 auto_code/README.md）：这里只有"程序 + 数据"——
TS 程序由 code_runtime 跑、断言由 python 跑，没有一行进 CMake。
"""
import argparse
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import time

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))          # 仓库根
BUILD = os.path.join(HERE, "build")                             # tsc 产物（code_runtime 的运行根）
RUNS = os.path.join(HERE, "runs")
CUR = os.path.join(RUNS, "current")
sys.path.insert(0, HERE)
from mcp import McpClient   # noqa: E402  （同目录的极简 MCP 客户端）

# 感知后端（多模态 chat/completions）由**驱动**给探针 —— 探针与被测实现都不内置地址。
# 默认是本机 llama.cpp 那个端点；换服务用 --vlm-url / 环境变量 JZ_VLM_URL、JZ_VLM_MODEL。
VLM_URL = os.environ.get("JZ_VLM_URL", "http://172.17.160.147:8080/v1/chat/completions")
VLM_MODEL = os.environ.get("JZ_VLM_MODEL", "my_model")


def bin_dir(cfg):
    return os.path.join(ROOT, "build", "binary", "code_agent", "bin", cfg)


def run(cmd, cwd=None, check=True, timeout=600):
    print("+ " + " ".join(cmd))
    p = subprocess.run(cmd, cwd=cwd, capture_output=True, timeout=timeout)
    out = (p.stdout or b"").decode("utf-8", "replace") + (p.stderr or b"").decode("utf-8", "replace")
    if out.strip():
        print(out.rstrip())
    if check and p.returncode != 0:
        print("FAIL: 命令返回 %d" % p.returncode)
        sys.exit(p.returncode)
    return p.returncode, out


def write_text(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def png_size(path):
    """读 PNG 的 IHDR（宽/高各 4 字节大端）—— 不引入任何依赖。"""
    with open(path, "rb") as f:
        head = f.read(24)
    if len(head) < 24 or head[:8] != b"\x89PNG\r\n\x1a\n":
        return (0, 0)
    return (int.from_bytes(head[16:20], "big"), int.from_bytes(head[20:24], "big"))


def kill_tree(pid):
    subprocess.run(["taskkill", "/F", "/T", "/PID", str(pid)], capture_output=True)


def window_size(text):
    """从 ui_windows 的回包取**当前目标窗口**的宽高（逻辑像素, 与真值同一坐标系）。"""
    try:
        d = json.loads(text)
        wins = d.get("windows") or []
        pick = None
        for w in wins:
            if w.get("current"):
                pick = w
                break
        if pick is None and wins:
            pick = wins[0]
        b = (pick or {}).get("bounds") or [0, 0, 0, 0]
        return (int(b[2]), int(b[3]))
    except Exception:
        return (0, 0)


def main():
    ap = argparse.ArgumentParser(description="computer_use 脚手架驱动")
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--level", type=int, default=None)
    ap.add_argument("--cfg", default="Debug")
    ap.add_argument("--skip-build", action="store_true")
    ap.add_argument("--no-archive", action="store_true")
    ap.add_argument("--no-shot", action="store_true",
                    help="跳过 MCP 截图（只出题/判分; 探针拿不到屏幕 ⇒ 必然全漏检）")
    ap.add_argument("--ui-port", type=int, default=0,
                    help="UI MCP 端口（默认自动挑一个空闲端口）")
    ap.add_argument("--vlm-url", default=VLM_URL,
                    help="感知后端（多模态 chat/completions）地址；--no-vlm 则不传（explore 会报缺少后端）")
    ap.add_argument("--vlm-model", default=VLM_MODEL, help="感知后端的模型名")
    ap.add_argument("--no-vlm", action="store_true", help="不给探针感知后端（验证'没后端要报错'这条契约）")
    args = ap.parse_args()

    cfg_dir = bin_dir(args.cfg)
    tsc = os.path.join(cfg_dir, "ts_compiler", "tsc", "tsc.exe")
    runtime = os.path.join(cfg_dir, "code_runtime.exe")
    for p, why in ((runtime, "code_runtime"), (tsc, "tsc")):
        if not os.path.exists(p):
            print("FAIL: 找不到 %s: %s（先构建设置 %s？）" % (why, p, args.cfg))
            return 1

    os.makedirs(CUR, exist_ok=True)

    # ---- 1) 编译 ----
    if not args.skip_build:
        run([tsc, "-p", os.path.join(HERE, "tsconfig.json")], cwd=HERE)

    # ---- 2) 写题目参数（种子/难度）----
    gen_cfg = os.path.join(BUILD, "scenes", "gen.json")
    prev = {}
    if os.path.exists(gen_cfg):
        try:
            prev = json.load(open(gen_cfg, encoding="utf-8"))
        except Exception:
            prev = {}
    seed = args.seed if args.seed is not None else int(prev.get("seed", 0))
    level = args.level if args.level is not None else int(prev.get("level", 0))
    # hold=true: 造完场景**不退出**, 当"假 App"进程留在屏幕上给驱动截图（见 scenes/gen.ts）
    write_text(gen_cfg, json.dumps({"seed": seed, "level": level, "hold": True}, ensure_ascii=False, indent=2))
    print("== case: seed=%d level=%d ==" % (seed, level))

    # ---- 3) 出题：造场景（真控件树）+ 导出真值 + 取一张屏幕 ----
    truth = os.path.join(HERE, "scenes", "truth.json")
    pred = os.path.join(CUR, "tree.json")
    shot = os.path.join(CUR, "shot.png")
    win_w, win_h = 0, 0
    if os.path.exists(truth):
        os.remove(truth)            # 先删旧真值: "这轮到底产出没有"要能判定
    gen_js = os.path.join(BUILD, "scenes", "gen.js")
    if args.no_shot:
        # 只需要题目（不要屏幕）: 前台跑一次即可, gen 会自己退出
        run([runtime, "--entry", gen_js], cwd=HERE, check=False)
    else:
        # 常驻 + MCP 取图: 窗口一出现 JZUiMcpBridge 才懒启动（端口=ui-port）
        port = args.ui_port or free_port()
        cmd = [runtime, "--debug", "--ui-mcp-port", str(port), "--entry", gen_js]
        print("+ " + " ".join(cmd))
        log = open(os.path.join(CUR, "scene_stdout.txt"), "wb")
        scene = subprocess.Popen(cmd, cwd=HERE, stdout=log, stderr=subprocess.STDOUT)
        try:
            c = McpClient(port)
            c.initialize()
            c.wait_ready(timeout=60)
            saved = c.screenshot_path()             # 目标窗口 → PNG（工具存在 exe 目录）
            shutil.copy2(saved, shot)
            win_w, win_h = window_size(c.windows())
            sw, sh = png_size(shot)
            print("== 屏幕: shot=%dx%d window=%dx%d scale=%.2f =="
                  % (sw, sh, win_w, win_h, (float(sw) / win_w) if win_w else 0.0))
        finally:
            kill_tree(scene.pid)
            try:
                scene.wait(timeout=30)
            except Exception:
                pass
            log.close()
    if not os.path.exists(truth):
        print("FAIL: 场景生成器没有产出 scenes/truth.json")
        return 1

    # ---- 4) 探索：探针产出 runs/current/tree.json（未实现时应给出明确原因, 而不是静默空跑）----
    probe = os.path.join(BUILD, "probe", "explore.js")
    if os.path.exists(probe):
        # 运行根 = 本 js 所在目录（build/probe）⇒ 配置放那儿（约定与 scenes/gen.json 一致）
        pcfg = {"screenshot": shot, "window": [win_w, win_h], "shot": list(png_size(shot)), "out": pred}
        if not args.no_vlm and args.vlm_url:
            pcfg["vlm"] = {"url": args.vlm_url, "model": args.vlm_model}
            print("== 感知后端: %s (model=%s) ==" % (args.vlm_url, args.vlm_model))
        write_text(os.path.join(BUILD, "probe", "probe.json"),
                   json.dumps(pcfg, ensure_ascii=False, indent=2))
        run([runtime, "--entry", probe], cwd=HERE, check=False)
    else:
        print("!! 探针还没编译出：%s（probe/explore.ts）" % probe)
    if not os.path.exists(pred):
        write_text(pred, json.dumps({"nodes": []}, ensure_ascii=False, indent=2))
        print("!! 已写空预测 -> 下面的判分结果会全是'漏检'（这是预期的红灯）")

    # ---- 5) 判分 ----
    rc, out = run([sys.executable, os.path.join(HERE, "verify.py"),
                   "--truth", truth, "--pred", pred], cwd=HERE, check=False)

    # ---- 6) 归档 ----
    if not args.no_archive:
        target = os.path.join(RUNS, "round_%03d" % (round_index() + 1))
        os.makedirs(target, exist_ok=True)
        for name, src in (("truth.json", truth),
                          ("scene.xml", os.path.join(CUR, "scene.xml")),
                          ("shot.png", shot),
                          ("tree.json", pred),
                          ("result.json", os.path.join(CUR, "result.json"))):
            if os.path.exists(src):
                shutil.copy2(src, os.path.join(target, name))
        with open(os.path.join(RUNS, "scores.jsonl"), "a", encoding="utf-8") as f:
            f.write(json.dumps({"round": os.path.basename(target), "seed": seed, "level": level,
                                "pass": ("PASS" in out.splitlines()), "ts": int(time.time())},
                               ensure_ascii=False) + "\n")
        print("== 归档 -> %s ==" % os.path.relpath(target, ROOT))

    print("== %s ==" % ("PASS" if rc == 0 else "FAIL"))
    return rc


def round_index():
    if not os.path.isdir(RUNS):
        return 0
    return len([d for d in os.listdir(RUNS) if d.startswith("round_")])


if __name__ == "__main__":
    sys.exit(main())
