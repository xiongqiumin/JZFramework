#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
game_play 任务 —— 环境侧（容器客户端 + 程序的生命周期 + 像素小工具）

这里只放"环境"这一侧的东西，一行策略都没有：
  · Container      —— virtual_device 的 HTTP 客户端（只认 /api/health、/api/windows、
                      /api/screen、/api/input/key、/api/programs*；接口权威定义见
                      binary/virtual_device/JZVirtualDeviceHttp.h）；
  · GameSession    —— 容器里"游戏机"程序（题目侧 gba.ts）的生命周期：加载 → 等窗口 →
                      截图 / 按键。**链路自检**（checks/chain_smoke.py）与外部证据都走它；
  · write_play_config —— 给容器里的 **play.ts**（最终产出的一键脚本）写参数
                      （build/play/play.json：产物目录/窗口标题/预热/步数/节拍）；
  · png / 像素工具 —— 纯 zlib 解码（不引第三方依赖）：非黑比例、两帧差异比例。

三条纪律（与 computer_use 的 loop-design 同一套）：
  1. **坐标恒为桌面/客户区逻辑像素**（无头下 scale 恒 1 ⇒ PNG 尺寸 == 客户区尺寸）；
  2. 容器**不给控件树**：脚本侧只能看像素（`__jz.computeruse.screenshot`）、只能操作键鼠，
     游戏内部状态一律读不到；
  3. 截图口径 = **窗口客户区**（`GET /api/screen?window=<句柄>`），与按键/命中同一坐标系。
"""

import json
import os
import socket
import struct
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import zlib

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))

# GBA 原生画面（1:1 上屏，不放大不超分 —— 见 JZEmulatorView.h 头注释）
GBA_W, GBA_H = 240, 160

# 容器里就是**两个进程**（= POST /api/programs 的 name）：
#   gba    = 题目侧的"游戏机"（game/gba.ts）：只开窗 + 跑 ROM，没有任何策略；
#   script = **最终产出的一键脚本**（play/play.ts）：用 computer use 自己看画面、自己按键。
# 脚本是"人"，gba 是"App" —— 两者只通过同一块屏幕和键盘发生关系。
GAME_PROGRAM = "gba"
PLAY_PROGRAM = "script"
WINDOW_TITLE = "GBA"


def write_play_config(script_dir, out, window=WINDOW_TITLE, warmup_sec=3.0,
                      steps=120, step_ms=400, max_seconds=0.0,
                      app=GAME_PROGRAM, play=""):
    """给容器里的脚本写参数（**写在脚本自己所在目录**，名字固定 play.json）。

    脚本按 `jzApp.applicationDirPath()`（= 它自己所在目录）找这份配置 —— 产出物是
    `build/play/play.js`、玩法自带的参考脚本是 `build/game/<玩法>/play.js`，各有各的根，
    所以这里要的是**脚本产物的目录**（传成"任务 build 目录"会写歪，脚本读不到就起不来）。

    字段与脚本的 PlayCfg 一一对应；路径一律**绝对路径**
    （脚本运行在容器线程里，工作目录不保证是谁的）。
    app  = 目标进程名（游戏机），由驱动按玩法给；
    play = 玩法名（hello / shooter / gba …）—— 脚本据此选按键映射与策略。
    """
    path = os.path.join(script_dir, "play.json")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    cfg = {"out": os.path.abspath(out), "app": app, "play": play, "window": window,
           "warmupSec": float(warmup_sec), "steps": int(steps), "stepMs": int(step_ms),
           "maxSeconds": float(max_seconds)}
    with open(path, "w", encoding="utf-8") as f:
        json.dump(cfg, f, ensure_ascii=False, indent=2)
    return path


def read_json(path, default=None):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return default


def wait_file(path, timeout=60.0, interval=0.2):
    """等一个文件出现（脚本收工时会写 session.json）——返回 True/False。"""
    end = time.time() + timeout
    while time.time() < end:
        if os.path.exists(path):
            return True
        time.sleep(interval)
    return False

# ==================== 按键名 ====================
# 策略侧说的是 **GBA 键名**（A/B/Start/Select/L/R + 方向），驱动在这一层翻成 Win32 键名
# 再发给容器（容器只认 Win32 名字，见 JZVirtualDeviceHttp.cpp 的 vkFromName）。
# 这张表**必须与 JZEmulatorView::gbaKeyOf 一致**（那是权威映射，改了要两边一起改）：
#   GBA A -> 'Z'   GBA B -> 'X'   Start -> Return   Select -> Shift   L -> 'A'   R -> 'S'
#   ⚠ "A"/"S" 这两个**字母**在容器里是 L/R，不是 GBA 的 A/B —— 用 GBA 键名就不会踩这个坑。
GBA_KEYS = {
    "a": "Z", "b": "X", "start": "Return", "select": "Shift",
    "l": "A", "r": "S",
    "up": "Up", "down": "Down", "left": "Left", "right": "Right",
}

# 容器认识的"原样名字"（给"我就想直接按 Win32 键"留的口子；见 vkFromName）
WINDOWS_KEYS = {
    "return", "enter", "tab", "escape", "esc", "backspace", "delete", "space", "insert",
    "home", "end", "pageup", "pagedown", "up", "down", "left", "right", "shift", "ctrl",
    "control", "alt", "capslock", "numlock", "scrolllock",
} | {"f%d" % i for i in range(1, 13)}


def key_of(name):
    """GBA 键名 / Win32 键名 -> 容器要的 Win32 键名。认不出来抛 ValueError（早报，别等 400）。"""
    n = (name or "").strip()
    low = n.lower()
    if low in GBA_KEYS:
        return GBA_KEYS[low]
    if low in WINDOWS_KEYS:
        return n if len(n) > 1 else n.upper()
    if len(n) == 1 and n.isalnum():
        return n.upper()
    raise ValueError("不认识的按键名: %r（用 GBA 键名 A/B/Start/Select/L/R/Up/Down/Left/Right，"
                     "或 Win32 键名如 Return/Shift/Z）" % name)


# ==================== 玩法（题目自包含：`game/<玩法>/config.json`） ====================
def load_task(base_dir=None):
    """读任务定义 task.json（玩法表 plays / 默认玩法 defaultPlay）—— **老任务可能没有这个文件**，
    没有就返回 {}（`play_config()` 会回落到 `game/<玩法>/config.json`）。"""
    return read_json(os.path.join(base_dir or HERE, "task.json"), {}) or {}


def play_config(play=None, cfg=None, base_dir=None):
    """取一个玩法的运行参数。

    两条路（**都不需要 task.json**）：
      · 直接读玩法目录：`play_config_dir(<test_dir>)`（新接口走的这条）；
      · 这条路：`play` = 玩法名 ⇒ 先查 task.json 的 `plays.<名>`（老任务/老写法），
        **查不到就回落 `<base_dir>/game/<名>/config.json`**（题目自包含；尺子只传 `--play <名>` 也照样能跑）。

    玩法表项有两种写法：
      · **目录式（推荐）** `{"dir": "game/shooter"}` —— 读该目录下的 `config.json` 当主体，
        表项里的其它字段再覆盖它。**玩法自包含**：游戏本体 / 尺子(verify.py) / 配置 都在那个目录里；
      · 内联式（老写法）`{"app": "...", "js": "...", "program": "...", ...}`。

    返回的字段：app(题目侧入口 ts) / js(编译产物) / program(进程名) / window(窗口标题) /
    desktop(容器桌面) / native(原生客户区尺寸) / rom(要不要 ROM) / verify(玩法自己的尺子) /
    以及玩法自己的其它字段（如 targetScore）。名字不认识直接抛错（别静默跑成另一个玩法）。
    """
    root = base_dir or HERE
    task = cfg if isinstance(cfg, dict) else load_task(root)
    plays = task.get("plays") or {}
    name = play or task.get("defaultPlay") or ""
    entry = plays.get(name)
    if not isinstance(entry, dict) and name and os.path.isdir(os.path.join(root, "game", name)):
        entry = {"dir": os.path.join("game", name)}     # 回落：题目自包含（没有 task.json 也认）
    if not isinstance(entry, dict):
        have = sorted(os.listdir(os.path.join(root, "game"))) if os.path.isdir(os.path.join(root, "game")) else []
        raise RuntimeError("不认识的玩法: %r（task.json 里有: %s；game/ 下有: %s）"
                           % (name, ", ".join(sorted(plays)) or "(没有 task.json)",
                              ", ".join(have) or "(没有 game/)"))
    out = {}
    if entry.get("dir"):
        pdir = entry["dir"] if os.path.isabs(entry["dir"]) else os.path.join(root, entry["dir"])
        out.update(read_json(os.path.join(pdir, "config.json"), {}) or {})
        out["dir"] = entry["dir"]
        out["dir_abs"] = pdir
    out.update({k: v for k, v in entry.items() if k != "dir"})
    out["play"] = name
    return out


def play_config_dir(test_dir, task_dir=None):
    """直接读一个**玩法目录**的 config.json —— 题目自包含时的入口（不再需要 task.json 的 plays 表）。

    返回与 `play_config()` 同形状的 dict，并补三个只有这里能给的字段：
      · `play`     = 玩法名（config.json 里写了就用它，否则取目录名）
      · `dir_abs`  = 玩法目录的绝对路径
      · `test_dir` = 同上（新接口里"题目目录"的名字）

    ⚠ 路径字段（`app` / `verify` / `js`）该目录里可能还是**任务根相对**的写法（如
      `game/hello/game.ts`），定位它们用 `play_file()`；`js` 是"产物相对路径"的提示，见
      `play_js_name()`。
    """
    d = os.path.abspath(test_dir)
    cfg = read_json(os.path.join(d, "config.json"), None)
    if not isinstance(cfg, dict):
        raise RuntimeError("玩法目录里没有 config.json（或不是 JSON 对象）: %s" % d)
    out = dict(cfg)
    out["play"] = out.get("play") or os.path.basename(d.rstrip("/\\"))
    out["dir_abs"] = d
    out["test_dir"] = d
    return out


def play_file(test_dir, value, task_dir=""):
    """题目目录里的一个文件在哪 —— `value` 可能是绝对路径、相对**题目目录**、或旧写法相对**任务根**。

    依次试：值本身（绝对）→ 题目目录/值 → 题目目录/basename(值) → 任务根/值。
    都不存在就返回"题目目录/值"（让调用方拿着这个不存在路径去报错，别静默换成别的题）。
    """
    if not value:
        return ""
    v = str(value).replace("\\", "/")
    if os.path.isabs(v):
        return os.path.normpath(v)
    cands = [os.path.join(test_dir, v), os.path.join(test_dir, os.path.basename(v))]
    if task_dir:
        cands.append(os.path.join(task_dir, v))
    for c in cands:
        if os.path.exists(c):
            return os.path.normpath(c)
    return os.path.normpath(cands[0])


def play_js_name(cfg, app_ts=""):
    """玩法本体的 JS **产物文件名**（新接口下产物落在工作区，布局自己说了算）。

    规则：config.json 的 `js` 是"产物相对路径"，取它的 basename；没写就 `basename(app).js`。
    （`jsRoot="repo"` 的玩法不走这里：产物在题目自己的工程里，路径由 `play_js_path()` 给。）
    """
    js = str(cfg.get("js") or "")
    if js:
        return os.path.basename(js.replace("\\", "/"))
    if app_ts:
        return os.path.splitext(os.path.basename(str(app_ts).replace("\\", "/")))[0] + ".js"
    return "game.js"


def play_js_path(cfg, task_dir=None, build_dir=None):
    """玩法本体的 JS 产物路径（容器加载的那份）。

    默认 `js` 是**相对任务 build/**的路径（本体在任务目录里）。
    配 `jsRoot: "repo"` 时改成**相对仓库根** —— 给"本体不在本任务里、直接复用别处的工程"
    （如 shooter 用 binary/code_agent/runtime/test_jsshooter）留的口子；绝对路径原样返回。
    """
    js = cfg.get("js") or ""
    if not js or os.path.isabs(js):
        return js
    if (cfg.get("jsRoot") or "task").lower() == "repo":
        return os.path.normpath(os.path.join(ROOT, js))
    return os.path.normpath(os.path.join(build_dir or os.path.join(task_dir or HERE, "build"), js))


def play_prebuild(cfg):
    """跑局之前要先编的**别的工程**（相对仓库根，如某个 tsconfig.json）→ 绝对路径列表。

    本任务自己的工程由驱动按 tsconfig 编，不用写在这儿；这里只列"本体住在别处"的工程
    （玩法 config.json 的 `prebuild` 字段）。
    """
    out = []
    for p in (cfg.get("prebuild") or []):
        out.append(p if os.path.isabs(p) else os.path.normpath(os.path.join(ROOT, p)))
    return out


# ==================== 路径推导 ====================
def container_exe(cfg="Debug"):
    return os.path.join(ROOT, "build", "binary", "virtual_device", "bin", cfg, "virtual_device.exe")


def runtime_bin_dir(cfg="Debug"):
    """code_runtime 那一堆产物所在目录：ts_compiler / jz_engine / **mgba_host.dll 与它的
    MinGW 运行库**都在这儿（JZEmulator 模块的 POST_BUILD 把它们拷到 exe 边上）。"""
    return os.path.join(ROOT, "build", "binary", "code_agent", "bin", cfg)


def emulator_dll_dir(cfg="Debug"):
    """mgba_host.dll 所在目录 —— 交给 JS 侧 `view.openRom(dllDir, rom)` 的第一个参数。

    ⚠ 容器 exe（virtual_device.exe）边上**不一定**有这几个 DLL（它只在接了 JZEmulator
    模块时才有），所以这里显式指到 code_runtime 目录，靠 JZEmulatorCore::load(dir) 的
    LOAD_WITH_ALTERED_SEARCH_PATH 从该目录找依赖（三个 MinGW 运行库必须与它同目录）。
    """
    return runtime_bin_dir(cfg)


def write_game_config(game_dir, dll_dir, rom, status_path=None, extra=None):
    """把玩法的运行参数写到它的**运行根**（= 编译产物所在目录，如 build/game 或
    build/game/shooter）下的 `game.json`，由题目侧程序自己读：

      · gba 玩法：dllDir / rom / statusPath（开窗 + 开 ROM + 状态上报）
      · shooter 玩法：只往 stdout 打印状态行、**不落状态文件**；通关线由玩法配置的
        `targetScore` 给（尺子自己判 score >= target，不必写进这里）

    extra 里的字段原样并进去（玩法自己的参数）。
    """
    os.makedirs(game_dir, exist_ok=True)
    cfg = {
        "dllDir": dll_dir,
        "rom": rom,
        "statusPath": status_path or os.path.join(HERE, "runs", "current", "game_status.json"),
    }
    if extra:
        cfg.update(extra)
    with open(os.path.join(game_dir, "game.json"), "w", encoding="utf-8") as f:
        json.dump(cfg, f, ensure_ascii=False, indent=2)
    return cfg


def tsc_exe(cfg="Debug"):
    return os.path.join(runtime_bin_dir(cfg), "ts_compiler", "tsc", "tsc.exe")


def runtime_exe(cfg="Debug"):
    return os.path.join(runtime_bin_dir(cfg), "code_runtime.exe")


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def kill_tree(pid):
    subprocess.run(["taskkill", "/F", "/T", "/PID", str(pid)], capture_output=True)


# ==================== 容器 HTTP 客户端 ====================
class Container:
    """极简 HTTP 客户端（响应体 {"ok":true,...} / {"ok":false,"error":...}；截图是 PNG 二进制）。"""

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

    # ---- 基础 ----
    def health(self):
        return self._json("GET", "/api/health")

    def wait_up(self, timeout=30, interval=0.3):
        end, last = time.time() + timeout, ""
        while time.time() < end:
            try:
                return self.health()
            except Exception as e:
                last = str(e)
            time.sleep(interval)
        raise RuntimeError("等容器起来超时(%ss): %s" % (timeout, last))

    # ---- 窗口 ----
    def windows(self, process=None):
        q = ("?process=" + urllib.parse.quote(process)) if process else ""
        return self._json("GET", "/api/windows" + q).get("windows") or []

    def programs(self):
        """程序表（GET /api/programs）-> [{"name":..,"windows":[..]}, ...]。

        用来证明"容器里就这两个进程"（gba + script）—— 证据里能看见，报告里也写出来。
        """
        return self._json("GET", "/api/programs").get("programs") or []

    def wait_window(self, process, timeout=60, interval=0.3):
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

    def activate(self, key):
        return self._json("POST", "/api/windows/%s/state" % urllib.parse.quote(str(key)),
                          {"action": "activate"})

    # ---- 截图 / 输入 ----
    def screen_png(self, window=None):
        q = ("?window=" + urllib.parse.quote(str(window))) if window else ""
        raw, ctype = self._req("GET", "/api/screen" + q)
        if "image/png" not in ctype.lower():
            raise RuntimeError("截图不是 PNG (%s): %s" % (ctype, raw[:200]))
        return raw

    def key(self, name, press=True, window=None):
        """按键：window 给了就走"直投那个窗口"（不激活）；不给走桌面（命中 + 激活）。"""
        body = {"key": name, "press": bool(press)}
        if window:
            return self._json("POST", "/api/windows/%s/input" % urllib.parse.quote(str(window)),
                              dict(body, type="key"))
        return self._json("POST", "/api/input/key", body)

    def tap(self, name, hold_ms=60, window=None):
        """按一下（按下 → 保持 → 松开）。GBA 的一帧 16.7ms，60ms 足够被跑到。"""
        a = self.key(name, True, window)
        time.sleep(hold_ms / 1000.0)
        b = self.key(name, False, window)
        return {"press": a, "release": b}

    # ---- 程序 ----
    def load_program(self, name, path, work_dir=None, args=None):
        body = {"name": name, "file": path}
        if work_dir:
            body["workDir"] = work_dir
        if args:
            body["args"] = list(args)      # 程序自己的启动参数(字符串数组) -> 程序里 process.argv[2..]
        return self._json("POST", "/api/programs", body)

    def load_script(self, name, script):
        return self._json("POST", "/api/programs", {"name": name, "script": script})

    def programs(self):
        return self._json("GET", "/api/programs").get("programs") or []

    def logs(self, offset=0, size=-1):
        """程序输出 (console.log 等) —— 容器侧环形缓冲里的**最近 1000 行**，按行取。

        offset = 起始**绝对行号**(累计行号; 给得太早 -> 从最老还留着的那行开始),
        size   = 要几行 (省略/负数 = 一直到末尾)。
        返回 {total, first, count, lines:[...]}；行文本形如 `[vproc: <进程名>] <原始输出>`。
        ⚠ 行号是**累计**的: 先取一次 total 当基线，之后 offset=基线 就只拿新增的行 ——
        比"容器 stdout 文件 + 字节偏移"干净(同一容器跑多轮时那边会累积, 还得自己算偏移)。
        """
        return self._json("GET", "/api/logs?offset=%d&size=%d" % (int(offset), int(size)))

    def stop_program(self, name):
        try:
            return self._json("DELETE", "/api/programs/" + urllib.parse.quote(name))
        except Exception:
            return None


class ContainerProc:
    """起/收一个无头容器进程（生命周期由驱动决定：无头下它不会自己退）。"""

    def __init__(self, exe, desktop, port, cwd, log_path):
        self.exe, self.desktop, self.port, self.cwd, self.log_path = exe, desktop, port, cwd, log_path
        self.proc = None
        self.log = None

    def start(self, timeout=30):
        cmd = [self.exe, "--headless", "--http-port", str(self.port), "--desktop", self.desktop]
        self.log = open(self.log_path, "wb")
        self.proc = subprocess.Popen(cmd, cwd=self.cwd, stdout=self.log, stderr=subprocess.STDOUT)
        c = Container(self.port)
        c.wait_up(timeout=timeout)
        return c

    def stop(self):
        if self.proc:
            kill_tree(self.proc.pid)
            try:
                self.proc.wait(timeout=30)
            except Exception:
                pass
            self.proc = None
        if self.log:
            self.log.close()
            self.log = None


# ==================== agent_cli（"拉取 agent"这一侧） ====================
class AgentCliProc:
    """拉起/收尾一个 **agent_cli** 进程（它对外是 HTTP 服务：POST /task）。

    只做三件事 —— 起进程、等它 HTTP 就绪、退出时收掉；**提交任务与循环结构不在这里**
    （那是上层的事，见 run.json 的 `agent` 段 / run.py）。

    就绪判定与 binary/agent_cli/tools/agent_loop.py 一致：轮询 GET /info，2xx 或
    "非 401 的 4xx" 即算起来；401 = token 不对，直接算失败（别把鉴权错当成没起来）。

    http 地址来源（优先显式给的）：
      ① 参数 http_url / token；② agent_cli 的 config.json（-c 指定的，否则 exe 同目录）；
      ③ 兜底 http://127.0.0.1:8080（agent_cli 的默认 http.bind / http.port）。
    """

    def __init__(self, bin_path, config_path="", http_url="", token="",
                 ready_sec=120, cwd=None, log_path=None):
        self.bin = bin_path
        self.config = config_path
        self.cwd = cwd or os.path.dirname(os.path.abspath(bin_path))
        self.log_path = log_path
        self.ready_sec = ready_sec
        self.proc = None
        self.logf = None
        self.url, self.token = self._resolve_http(http_url, token)

    def _cli_config_path(self):
        if self.config:
            return self.config
        return os.path.join(os.path.dirname(os.path.abspath(self.bin)), "config.json")

    def _resolve_http(self, http_url, token):
        url, tk = http_url, token
        if not url or not tk:
            cfg = read_json(self._cli_config_path(), {}) or {}
            http = cfg.get("http") if isinstance(cfg.get("http"), dict) else {}
            bind = http.get("bind") or cfg.get("httpBind") or "127.0.0.1"
            port = http.get("port") or cfg.get("httpPort") or 8080
            if not url:
                url = "http://%s:%d" % (bind, port)
            if not tk:
                tk = http.get("token") or cfg.get("httpToken") or ""
        return url.rstrip("/"), tk

    def start(self, log_path=None):
        """起进程并等就绪；返回是否就绪（起不来/超时/401 都返回 False，细节看日志）。"""
        if log_path:
            self.log_path = log_path
        cmd = [self.bin]
        if self.config:
            cmd += ["-c", self.config]
        if self.log_path:
            os.makedirs(os.path.dirname(self.log_path), exist_ok=True)
            self.logf = open(self.log_path, "w", encoding="utf-8")
        print("+ " + " ".join(cmd))
        self.proc = subprocess.Popen(cmd, cwd=self.cwd, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT)
        threading.Thread(target=self._pump, daemon=True).start()
        return self.wait_ready()

    def _pump(self):
        for raw in self.proc.stdout:
            line = raw.decode("utf-8", "replace").rstrip()
            print("[agent_cli] " + line)
            if self.logf:
                self.logf.write(line + "\n")
                self.logf.flush()

    def _info_code(self):
        req = urllib.request.Request(self.url + "/info")
        if self.token:
            req.add_header("Authorization", "Bearer " + self.token)
        with urllib.request.urlopen(req, timeout=3) as r:
            return r.status

    def wait_ready(self, timeout=None):
        end = time.time() + (timeout if timeout else self.ready_sec)
        while time.time() < end:
            try:
                if 200 <= self._info_code() < 300:
                    return True
            except urllib.error.HTTPError as ex:
                if ex.code == 401:
                    print("!! agent_cli 起来了但 token 不对(401): %s" % self.url)
                    return False
                if 400 <= ex.code < 500:   # 非 401 的 4xx: 服务在, 只是这个请求它不认
                    return True
            except Exception:
                pass
            if self.proc and self.proc.poll() is not None:
                print("!! agent_cli 退出码 %s —— 没起来, 看日志" % self.proc.returncode)
                return False
            time.sleep(0.5)
        print("!! agent_cli 等不到就绪(%ds): %s" % (self.ready_sec, self.url))
        return False

    def submit_task(self, prompt, timeout=7200, extra=None):
        """POST /task 提交一轮任务；**阻塞**到这一轮结束，返回回包 dict。

        回包字段（详见 JZAgentCli.h）：status(success/failed/cancelled)、verify_rounds、
        agent_reply、last_verify_output、error…。
        ⚠ 不给 verify_cmd：agent 这一轮只"干活"，验证由驱动（run.py 跑一局 + verify.py）做 ——
          否则 agent_cli 会自己再跑一遍驱动，递归。
        网络/HTTP 错直接抛（调用方决定重试还是收工）。
        """
        body = {"prompt": prompt}
        if extra:
            body.update(extra)
        data = json.dumps(body, ensure_ascii=False).encode("utf-8")
        req = urllib.request.Request(self.url + "/task", data=data,
                                     headers={"Content-Type": "application/json"})
        if self.token:
            req.add_header("Authorization", "Bearer " + self.token)
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read().decode("utf-8", "replace"))

    def alive(self):
        """进程还在不在（进程在、但 HTTP 不通 = 卡住，由调用方判）。"""
        return bool(self.proc) and self.proc.poll() is None

    def info(self, timeout=5):
        """GET /info → dict（读 busy / round / mcp 等；连不上或鉴权错 → {}）。"""
        req = urllib.request.Request(self.url + "/info")
        if self.token:
            req.add_header("Authorization", "Bearer " + self.token)
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                return json.loads((r.read() or b"{}").decode("utf-8", "replace"))
        except Exception:
            return {}

    def busy(self):
        """agent 手头还有任务在跑吗（/info 的 busy 字段）。"""
        return bool(self.info().get("busy"))

    def wait_idle(self, timeout=1800.0, interval=5.0):
        """等 agent 把手头的任务跑完（busy=False）。

        为什么要等：`POST /task` 是**连接保持**式的 —— 提交方（我们）超时断开**不会**杀掉
        agent 手里的任务（JZAgentCli::onClientDestroyed 只把回包通道置空，任务照跑）。这时它
        可能还在改 play/play.ts；直接编译/跑局就是拿一个**还在变**的文件做实验。
        返回 True = 已空闲（或进程已不在），False = 等到超时。
        """
        end = time.time() + timeout
        while time.time() < end:
            if not self.alive():
                return True
            if not self.busy():
                return True
            time.sleep(interval)
        return False

    def stop(self):
        if self.proc:
            kill_tree(self.proc.pid)
            try:
                self.proc.wait(timeout=30)
            except Exception:
                pass
            self.proc = None
        if self.logf:
            self.logf.close()
            self.logf = None


def press_keys(c, window_key, keys, hold_ms=60):
    """按一次"组合键"：所有 keys **同时**按下 → 保持 hold_ms → 一起松开。

    keys 用 **GBA 键名**（A/B/Start/Select/L/R/Up/Down/Left/Right），这里翻成容器认的
    Win32 键名（映射的权威是 JZEmulatorView::gbaKeyOf，见 key_of）。
    返回 {"gbaKeys":[...], "winKeys":[...], "holdMs":.., "handled":bool, "error":str|None}。
    """
    try:
        win_keys = [key_of(k) for k in keys]
    except ValueError as ex:
        return {"gbaKeys": list(keys), "winKeys": [], "holdMs": hold_ms,
                "handled": False, "error": str(ex)}
    out = {"gbaKeys": list(keys), "winKeys": win_keys, "holdMs": hold_ms,
           "handled": True, "error": None}
    for k in win_keys:
        r = c.key(k, True, window_key)
        out["handled"] = out["handled"] and bool(r.get("handled"))
    time.sleep(max(0, hold_ms) / 1000.0)
    for k in win_keys:
        r = c.key(k, False, window_key)
        out["handled"] = out["handled"] and bool(r.get("handled"))
    return out


class HeadlessContainer:
    """无头容器的生命周期（起进程 → 等 health → 用；退出先删程序再收进程）。

    与 GameSession 的分工：GameSession 是"我要开一个游戏机来测链路"（checks 用），
    这里是"我只要一台容器，往里装任意程序"（run.py 用：装 game + 装 play）。
    """

    def __init__(self, cfg="Debug", desktop="320x240", exe="", log_path=None):
        self.exe = exe or container_exe(cfg)
        self.desktop = desktop
        self.log_path = log_path or os.path.join(HERE, "runs", "current", "container_stdout.txt")
        self.port = free_port()
        self.c = Container(self.port)
        self.proc = None
        self.programs = []
        self.logf = None
        self.log_lines = []

    def _on_log(self, line):
        self.log_lines.append(line)
        print(line)
        if self.logf:
            self.logf.write(line + "\n")
            self.logf.flush()

    def __enter__(self):
        os.makedirs(os.path.dirname(self.log_path), exist_ok=True)
        cmd = [self.exe, "--headless", "--http-port", str(self.port), "--desktop", self.desktop]
        print("+ " + " ".join(cmd))
        self.logf = open(self.log_path, "w", encoding="utf-8")
        self.proc = subprocess.Popen(cmd, cwd=os.path.dirname(self.log_path),
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        threading.Thread(target=self._pump, daemon=True).start()
        self.c.wait_up(timeout=30)
        return self

    def _pump(self):
        for raw in self.proc.stdout:
            self._on_log(raw.decode("utf-8", "replace").rstrip())

    def load(self, name, js, work_dir=None, args=None):
        r = self.c.load_program(name, js, work_dir or os.path.dirname(js), args)
        if name not in self.programs:
            self.programs.append(name)
        return r

    def __exit__(self, *exc):
        for name in reversed(self.programs):
            try:
                self.c.stop_program(name)
            except Exception:
                pass
        if self.proc:
            kill_tree(self.proc.pid)
            try:
                self.proc.wait(timeout=30)
            except Exception:
                pass
        if self.logf:
            self.logf.close()
        return False


class GameSession:
    """一局游戏：容器 + 里面那个"游戏机"程序（窗口 = 240x160 的 JZEmulatorView）。

    用法:
        with GameSession(cfg="Debug", rom=..., dll_dir=..., desktop="320x240") as g:
            img = g.shot("runs/current/frames/0001.png")
            g.press("Right", hold_ms=400)
    """

    def __init__(self, cfg="Debug", rom="", dll_dir="", desktop="320x240",
                 name=GAME_PROGRAM, container=None, cwd=None, log_path=None,
                 game_dir=None, on_log=None):
        self.cfg = cfg
        self.rom = rom
        self.dll_dir = dll_dir or emulator_dll_dir(cfg)
        self.desktop = desktop
        self.name = name
        # ⚠ 容器进程的**工作目录**落在 runs/current：模拟器宿主 DLL 会随手写点日志文件，
        #   别让它们掉在任务根（那会变成"没人知道该不该入库的垃圾"）。
        self.cwd = cwd or os.path.join(HERE, "runs", "current")
        self.log_path = log_path or os.path.join(HERE, "runs", "current", "container_stdout.txt")
        self.exe = container or container_exe(cfg)
        # 编译产物在 build/game/game.js（见 tsconfig.json: outDir=build, rootDir=.）
        self.game_dir = game_dir or os.path.join(HERE, "build", "game")
        self.on_log = on_log              # 可选: 每步打印一行（驱动用）
        self.proc = None
        self.c = None
        self.win = None
        self.window_key = None

    # ---- 生命周期 ----
    def __enter__(self):
        return self.open()

    def __exit__(self, *exc):
        self.close()
        return False

    def log(self, text):
        if self.on_log:
            self.on_log(text)

    def open(self):
        os.makedirs(os.path.dirname(self.log_path), exist_ok=True)
        os.makedirs(self.cwd, exist_ok=True)
        self.proc = ContainerProc(self.exe, self.desktop, free_port(), self.cwd, self.log_path)
        self.c = self.proc.start()
        self.load_game()
        return self

    def load_game(self):
        """把"游戏机"当 JS 程序加载进容器，等它的窗口，量一次客户区。

        游戏机 = `game/gba.ts`（编译到 build/game/gba.js）：它开一个 240x160 的窗口，
        里面是 JZEmulatorView；DLL 目录与 ROM 由运行根下的 `game.json` 给（驱动写）。
        """
        js = os.path.join(self.game_dir, "gba.js")
        if not os.path.exists(js):
            raise RuntimeError("找不到游戏机程序: %s（先编译：tsc -p auto_code/game_play/tsconfig.json）" % js)
        write_game_config(self.game_dir, self.dll_dir, self.rom)
        self.c.load_program(self.name, js, work_dir=self.game_dir)
        self.win = self.c.wait_window(self.name, timeout=60)
        # 句柄最稳（下标会随建窗/关窗变）；按键与截图都用它
        self.window_key = self.win.get("id") or self.win.get("index")
        self.c.activate(self.window_key)          # 置顶 + 激活：桌面那条输入路也要它在前台
        client = self.win.get("client") or [0, 0, 0, 0]
        self.client_size = (int(client[2]), int(client[3]))
        self.log("== 游戏窗口: title=%s client=%s 句柄=%s =="
                 % (self.win.get("title", ""), client, self.window_key))
        # 起点状态：等程序把状态文件写出来（它跟开 ROM 同一段代码，通常一瞬间就有）——
        # 这样"帧数从多少涨到多少"才有起点可读（尺子 G3 要用）。
        self.status0 = self.wait_status(timeout=5.0)
        return self.win

    def wait_status(self, timeout=5.0, interval=0.2):
        end = time.time() + timeout
        while True:
            st = self.game_status()
            if "frames" in st:
                return st
            if time.time() >= end:
                return st
            time.sleep(interval)

    def game_status(self):
        """读游戏机**自己写的**状态文件（`game.json.statusPath`，由题目侧程序写）。

        用途：把"ROM 没打开 / 定时器没在跑"这类**链路故障**与"游戏玩得不好"分开
        —— 后者不归尺子管（这一版不自动判分，见 verify.py 的头注释）。
        ⚠ 它是题目侧程序写的**自报值**，只当诊断用；硬门里真正算数的是像素。
        """
        path = os.path.join(self.game_dir, "game.json")
        try:
            with open(path, encoding="utf-8") as f:
                status_path = json.load(f).get("statusPath")
            with open(status_path, encoding="utf-8") as f:
                return json.load(f)
        except Exception as e:
            return {"error": "读状态失败: %s" % e}

    def close(self):
        if self.c:
            self.c.stop_program(self.name)
        if self.proc:
            self.proc.stop()

    # ---- 观测 / 操作 ----
    def shot(self, path):
        """取一张客户区 PNG 落盘，返回 (path, (w, h))。"""
        raw = self.c.screen_png(self.window_key)
        os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
        with open(path, "wb") as f:
            f.write(raw)
        return path, png_size(raw)

    def press(self, name, hold_ms=60, direct=True):
        """按一下（name 用 **GBA 键名** A/B/Start/...；翻译见 key_of）。"""
        key = self.window_key if direct else None
        return self.c.tap(key_of(name), hold_ms=hold_ms, window=key)

    def hold(self, name, ms=300, direct=True):
        """按住 ms 毫秒再松开（走一步要用持续方向键时用这个）。"""
        key = self.window_key if direct else None
        win = key_of(name)
        self.c.key(win, True, key)
        time.sleep(ms / 1000.0)
        self.c.key(win, False, key)
        return ms


# ==================== PNG / 像素（不引依赖） ====================
def png_size(data):
    if len(data) < 24 or data[:8] != b"\x89PNG\r\n\x1a\n":
        return (0, 0)
    return (int.from_bytes(data[16:20], "big"), int.from_bytes(data[20:24], "big"))


def png_decode(data):
    """极简 PNG 解码 → (w, h, rgba: bytearray)。

    只支持 8bit、无隔行（GDI+ 编码器给的就是这一种）：colortype 0/2/4/6。
    够用就够用：这里只要"逐像素比差异 / 数非黑像素"，不需要完整 PNG 实现。
    """
    if len(data) < 8 or data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("不是 PNG")
    pos, idat, ihdr = 8, bytearray(), None
    while pos + 8 <= len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        ctype = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if ctype == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", body)
        elif ctype == b"IDAT":
            idat += body
        elif ctype == b"IEND":
            break
    if not ihdr:
        raise ValueError("PNG 没有 IHDR")
    w, h, depth, color, comp, filt, interlace = ihdr
    if depth != 8 or interlace != 0:
        raise ValueError("只支持 8bit 无隔行 PNG (depth=%d interlace=%d)" % (depth, interlace))
    chan = {0: 1, 2: 3, 4: 2, 6: 4}.get(color)
    if chan is None:
        raise ValueError("不支持的 colortype: %d" % color)

    raw = zlib.decompress(bytes(idat))
    stride = w * chan
    out = bytearray(w * h * 4)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        ftype = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride
        if ftype == 1:            # Sub
            for i in range(chan, stride):
                line[i] = (line[i] + line[i - chan]) & 0xFF
        elif ftype == 2:          # Up
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ftype == 3:          # Average
            for i in range(stride):
                a = line[i - chan] if i >= chan else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif ftype == 4:          # Paeth
            for i in range(stride):
                a = line[i - chan] if i >= chan else 0
                b = prev[i]
                c = prev[i - chan] if i >= chan else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        elif ftype != 0:
            raise ValueError("不认识的 PNG filter: %d" % ftype)
        prev = line
        o = y * w * 4
        for x in range(w):
            s = x * chan
            d = o + x * 4
            if chan == 1:
                out[d] = out[d + 1] = out[d + 2] = line[s]
                out[d + 3] = 255
            elif chan == 2:
                out[d] = out[d + 1] = out[d + 2] = line[s]
                out[d + 3] = line[s + 1]
            elif chan == 3:
                out[d:d + 3] = line[s:s + 3]
                out[d + 3] = 255
            else:
                out[d:d + 4] = line[s:s + 4]
    return w, h, out


def read_png(path):
    with open(path, "rb") as f:
        return png_decode(f.read())


def nonblack_ratio(img, thresh=24):
    """非黑像素比例 —— "ROM 真的跑起来了"的最低门槛（黑屏 = 没画面）。"""
    _, _, px = img
    n = len(px) // 4
    hit = 0
    for i in range(0, len(px), 4):
        if px[i] > thresh or px[i + 1] > thresh or px[i + 2] > thresh:
            hit += 1
    return hit / n if n else 0.0


def diff_ratio(a, b, thresh=16):
    """两帧差异像素比例 —— "画面在动 / 输入有反应"的观测量。"""
    if a[0] != b[0] or a[1] != b[1]:
        return 1.0
    pa, pb = a[2], b[2]
    n = len(pa) // 4
    hit = 0
    for i in range(0, len(pa), 4):
        if (abs(pa[i] - pb[i]) > thresh or abs(pa[i + 1] - pb[i + 1]) > thresh
                or abs(pa[i + 2] - pb[i + 2]) > thresh):
            hit += 1
    return hit / n if n else 0.0


def unique_colors(img, limit=4096):
    """画面里出现过的颜色数（上限 limit 就停）—— 黑屏 / 花屏的快速区分。"""
    _, _, px = img
    seen = set()
    for i in range(0, len(px), 4):
        seen.add((px[i], px[i + 1], px[i + 2]))
        if len(seen) >= limit:
            break
    return len(seen)
