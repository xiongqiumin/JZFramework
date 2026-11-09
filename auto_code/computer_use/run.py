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
  python run.py --container          # 屏幕改用**虚拟桌面容器**（无头, 见下面"两种屏幕"）

两种"屏幕"（其余步骤一行都不改）:
  · 真窗口路（默认）: 把场景进程常驻拉起（`--debug --ui-mcp-port N`，窗口一出现才起 UI MCP），
    用 `mcp.py` 的 `ui_screenshot` 取 PNG 存 `runs/current/shot.png`
    （截图 840x640 = 2 倍缩放，窗口逻辑尺寸 420x320 由 `ui_windows` 的 bounds 给出）；
  · 容器路（`--container`）: 起 `virtual_device.exe --headless --http-port N`，
    `POST /api/programs` 把场景当 **JS 程序**加载，`GET /api/screen?window=` 取客户区 PNG
    —— 无头下 scale 恒 1 ⇒ **PNG 尺寸 == 客户区逻辑尺寸**，不用算缩放；也不需要真桌面。
  ⚠ 两条路的截图口径都是**客户区**（真值 rect / hitTest 同一坐标系），探针只吃像素 + 窗口几何。

流程要点:
  - 出题 = `scenes/gen.ts`（一份声明 → uiLoader 真控件树 + 同源 truth.json）；
  - 屏幕 = 上面两条路之一 —— 探针只看像素，不看窗口内部；
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
import urllib.error
import urllib.parse
import urllib.request

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
# ⚠ 端点/模型名不入库(2026-11-09)：默认留空，用 --vlm-url/--vlm-model 或
# 环境变量 JZ_VLM_URL、JZ_VLM_MODEL 提供；没给就跑无感知后端的路径。
VLM_URL = os.environ.get("JZ_VLM_URL", "")
VLM_MODEL = os.environ.get("JZ_VLM_MODEL", "")


def bin_dir(cfg):
    # binary/ 下的 exe(code_agent / code_runtime / agent_cli / virtual_device ...)统一在这儿
    # (见 binary/CMakeLists.txt 的 CMAKE_RUNTIME_OUTPUT_DIRECTORY)。
    return os.path.join(ROOT, "build", "binary", "bin", cfg)


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


# ==================== 容器路（virtual_device --headless） ====================
# 设计见 loop-design.md §2; 接口的权威定义在 binary/virtual_device/JZVirtualDeviceHttp.h。
# 这里只用三个动作: 加载程序 / 查窗口 / 取客户区截图。
# ⚠ 容器**刻意不提供控件树**(给了就等于开卷, 见该头文件"五条纪律"第 3 条) ——
#   除了像素与窗口几何, 这里什么都不从容器里读; 几何是**驱动**写进 probe.json 的坐标系信息。

DESKTOP_DEFAULT = "1024x768"   # 场景窗口 420x320 上下, 给足空间（窗口摆哪不影响客户区截图）


def container_exe(cfg):
    return os.path.join(ROOT, "build", "binary", "bin", cfg, "virtual_device.exe")


def task_meta():
    """task.json —— 人与工具共读的任务清单（这里只取容器那几个字段, 缺了就用默认）。"""
    try:
        with open(os.path.join(HERE, "task.json"), encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return {}


class Container:
    """极简 HTTP 客户端（响应体 {"ok":true,...} / {"ok":false,"error":...}; 截图是 PNG 二进制）。"""

    def __init__(self, port, timeout=60):
        self.base = "http://127.0.0.1:%d" % port
        self.timeout = timeout

    def _req(self, method, path, body=None):
        data = None
        if body is not None:
            data = json.dumps(body, ensure_ascii=False).encode("utf-8")
        req = urllib.request.Request(self.base + path, data=data, method=method)
        if data is not None:
            req.add_header("Content-Type", "application/json")
        with urllib.request.urlopen(req, timeout=self.timeout) as resp:
            return resp.read(), (resp.headers.get("Content-Type") or "")

    def _json(self, method, path, body=None):
        raw, ctype = self._req(method, path, body)
        if "json" not in ctype.lower():
            raise RuntimeError("%s %s 没回 JSON (%s): %s" % (method, path, ctype, raw[:200]))
        d = json.loads(raw.decode("utf-8", "replace"))
        if isinstance(d, dict) and d.get("ok") is False:
            raise RuntimeError("%s %s 失败: %s" % (method, path, d.get("error")))
        return d

    def health(self):
        return self._json("GET", "/api/health")

    def wait_up(self, timeout=30, interval=0.3):
        """等容器把端口听上（进程起来到 listen 之间有一小段）。"""
        end, last = time.time() + timeout, ""
        while time.time() < end:
            try:
                return self.health()
            except Exception as e:
                last = str(e)
            time.sleep(interval)
        raise RuntimeError("等容器起来超时(%ss): %s" % (timeout, last))

    def load_program(self, name, path, work_dir=None):
        """加载场景: file = 一个 JS 程序（按 ESM 模块加载, 运行根 = 它所在目录）。"""
        body = {"name": name, "file": path}
        if work_dir:
            body["workDir"] = work_dir
        return self._json("POST", "/api/programs", body)

    def stop_program(self, name):
        try:
            return self._json("DELETE", "/api/programs/" + urllib.parse.quote(name))
        except Exception:
            return None

    def windows(self, process=None):
        q = ("?process=" + urllib.parse.quote(process)) if process else ""
        return self._json("GET", "/api/windows" + q).get("windows") or []

    def wait_window(self, process, timeout=60, interval=0.4):
        """等程序把窗口建出来（它要起 JS + uiLoader, 不是瞬时的）, 返回窗口对象。

        窗口字段见 JZVirtualDeviceHttp::windowJson: id(句柄)/title/frame/client/process...
        client = 客户区 [x,y,w,h]，桌面逻辑像素 —— 与真值 rect 同一坐标系。
        """
        end, last = time.time() + timeout, "窗口还没出现"
        while time.time() < end:
            try:
                wins = self.windows(process)
                if wins:
                    return wins[0]
            except Exception as e:
                last = str(e)
            time.sleep(interval)
        raise RuntimeError("等程序 %s 的窗口超时(%ss): %s" % (process, timeout, last))

    def screen_png(self, window=None):
        """客户区 PNG（不给 window 就是整屏）。"""
        q = ("?window=" + urllib.parse.quote(str(window))) if window else ""
        raw, ctype = self._req("GET", "/api/screen" + q)
        if "image/png" not in ctype.lower():
            raise RuntimeError("截图不是 PNG (%s): %s" % (ctype, raw[:200]))
        return raw


def shot_from_container(exe, desktop, port, gen_js, shot, name, timeout=60):
    """容器路取一张屏幕: 起容器 → 加载场景程序 → 取客户区 PNG → 收尾。

    返回 (win_w, win_h) = 客户区逻辑尺寸（无头下 scale 恒 1 ⇒ 与 PNG 尺寸相同）。
    与真窗口路的差别只有"屏幕从哪来": 这里不需要真桌面、不需要 MCP。
    """
    cmd = [exe, "--headless", "--http-port", str(port), "--desktop", desktop]
    print("+ " + " ".join(cmd))
    log = open(os.path.join(CUR, "container_stdout.txt"), "wb")
    proc = subprocess.Popen(cmd, cwd=HERE, stdout=log, stderr=subprocess.STDOUT)
    try:
        c = Container(port)
        c.wait_up(timeout=30)
        c.load_program(name, gen_js, work_dir=os.path.dirname(gen_js))
        w = c.wait_window(name, timeout=timeout)
        key = w.get("id") or w.get("index")            # 句柄最稳, 退回下标
        with open(shot, "wb") as f:
            f.write(c.screen_png(key))
        client = w.get("client") or [0, 0, 0, 0]
        print("== 容器窗口: title=%s client=%s ==" % (w.get("title", ""), client))
        return int(client[2]), int(client[3])
    finally:
        Container(port).stop_program(name)             # 先收程序（连带它建的窗口）
        kill_tree(proc.pid)                            # 无头容器不会自己退, 生命周期由驱动决定
        try:
            proc.wait(timeout=30)
        except Exception:
            pass
        log.close()


def main():
    ap = argparse.ArgumentParser(description="computer_use 脚手架驱动")
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--level", type=int, default=None)
    ap.add_argument("--cfg", default="Debug")
    ap.add_argument("--skip-build", action="store_true")
    ap.add_argument("--no-archive", action="store_true")
    ap.add_argument("--no-shot", action="store_true",
                    help="跳过截图（只出题/判分; 探针拿不到屏幕 ⇒ 必然全漏检）")
    ap.add_argument("--container", action="store_true",
                    help="屏幕改用虚拟桌面容器（virtual_device --headless: 无头, 不需要真桌面）")
    ap.add_argument("--container-exe", default="",
                    help="容器 exe 路径（默认 build/binary/bin/<cfg>/virtual_device.exe）")
    ap.add_argument("--desktop", default="",
                    help="容器桌面尺寸 WxH（默认取 task.json 的 container.desktop, 其次 %s）" % DESKTOP_DEFAULT)
    ap.add_argument("--ui-port", type=int, default=0,
                    help="UI MCP 端口（默认自动挑一个空闲端口；容器路不用）")
    ap.add_argument("--vlm-url", default=VLM_URL,
                    help="感知后端（多模态 chat/completions）地址；--no-vlm 则不传（explore 会报缺少后端）")
    ap.add_argument("--vlm-model", default=VLM_MODEL, help="感知后端的模型名")
    ap.add_argument("--no-vlm", action="store_true", help="不给探针感知后端（验证'没后端要报错'这条契约）")
    args = ap.parse_args()

    cfg_dir = bin_dir(args.cfg)
    tsc = os.path.join(cfg_dir, "ts_compiler", "tsc", "tsc.exe")
    runtime = os.path.join(cfg_dir, "code_runtime.exe")
    meta = task_meta()
    cconf = meta.get("container") or {}
    container = args.container_exe or container_exe(args.cfg)
    desktop = args.desktop or cconf.get("desktop") or DESKTOP_DEFAULT
    need = [(runtime, "code_runtime"), (tsc, "tsc")]
    if args.container:
        need.append((container, "virtual_device（容器）"))
    for p, why in need:
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
    # hold=true: 造完场景**不退出**, 当"假 App"留在屏幕上给驱动截图 / 给容器加载（见 scenes/gen.ts）。
    # --no-shot 时没有"外面"要看它, 让它自己退 —— 否则前台那次调用会一直挂着。
    write_text(gen_cfg, json.dumps({"seed": seed, "level": level, "hold": not args.no_shot},
                                   ensure_ascii=False, indent=2))
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
    elif args.container:
        # 容器路: 起一台无头虚拟机 → 把场景当 JS 程序加载 → 取客户区 PNG
        win_w, win_h = shot_from_container(container, desktop, free_port(), gen_js, shot,
                                           cconf.get("program", "scene"))
        sw, sh = png_size(shot)
        print("== 屏幕: shot=%dx%d window=%dx%d scale=%.2f =="
              % (sw, sh, win_w, win_h, (float(sw) / win_w) if win_w else 0.0))
    else:
        # 真窗口路: 常驻 + MCP 取图（窗口一出现 JZUiMcpBridge 才懒启动, 端口=ui-port）
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
