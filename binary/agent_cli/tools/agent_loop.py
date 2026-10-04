#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""agent_cli 无人值守驱动 + 守护: 拉起 agent_cli(HTTP) -> 逐轮 POST /task -> 收工复验 -> 崩了重拉。

work 目录 = 本脚本所在目录(宿主机 ~/docker/work, 容器内 /home/nas/work);
路径一律用"相对 work 目录"的写法, 两个环境都能直接跑。

为什么是"HTTP 逐轮提交"而不是给 agent_cli 加个 --loop 模式:
    一轮 = 一次 POST /task(prompt + verify_cmd), 走的就是网页提交那条链路(同一会话、同一
    "验证脚本门控"循环)。跑几轮 / 什么时候停本来就是**提交方**的决策 —— 验证程序在自己输出里
    打 [loop-stop], 这里看见后不再提交下一轮即可。所以 CLI 侧不需要第二套循环模式, 也不需要
    认识任何标记(一套链路一个实现), 顺带还能边跑边用网页看过程。

一轮里发生什么(submitTask, 与网页提交完全一致):
    prompt -> agent 干活 -> 跑验证程序
        退出码≠0 -> 输出**原样喂回模型**续跑(验证脚本门控), 直到通过或用完 max_verify_rounds
        退出码=0 -> 结果里带回 last_verify_output(本脚本据此判断"要不要再来一轮")
    验证程序自己决定"这一轮之后做什么": 复制备份 / 继续改 / 到此为止(模板见 test/primes_verify.py)。

本脚本负责"进程 + 轮次 + 复验"这三件事:
    1. 拉起 agent_cli, 等它的 HTTP 就绪, 然后逐轮提交(--verify-cmd 进请求体);
    2. 每轮往 supervisor.log 记一行(状态 / 验证轮次 / 用时 / 验证输出末行);
    3. 某轮验证输出里出现 --stop-tag(默认 [loop-stop]) ⇒ 不再提交下一轮, 先跑 --recheck-cmd
       外部复验(同一个验证程序 + 只判定不落盘的开关), 通过才收工(退出码 0); 不通过就当没达成,
       重拉继续(最多 --recheck-retries 次, 免得反复空转);
    4. agent_cli 崩了(被杀 / 崩 / 端口没起来 / 提交中断) ⇒ 记退出码与运行时长, 等 retry-sec 秒
       重拉, 起来后接着提交(会话重新开始, 但工作区里的东西都还在);
    5. 生命周期与本进程绑定: 本脚本消失或被 Ctrl+C, 一定连子孙一起收掉。

由 run_loop.sh 负责 {start|stop}; 也可以直接跑:
    python3 agent_loop.py --dry-run                          # 先看会往哪个 URL 提交什么
    python3 agent_loop.py --verify-cmd "<python> test/primes_verify.py ..."
    python3 agent_loop.py --max-rounds 3                     # 只跑 3 轮就收工(验证用)

产物(默认 <脚本目录>/logs, 可用 --log-dir 或环境变量 LOG_DIR 改):
    agent_cli_YYYYMMDD.log    agent_cli 原始输出(stdout+stderr 合并), 单文件上限 max-log-mb,
                              到上限自动续写 agent_cli_YYYYMMDD_2.log / _3.log ...; 跨天自动换新文件
    supervisor.log            每次 启动/退出/轮次/复验/日志轮转/等待 的一行记录(每轮一行 ROUND ...)
    agent_loop.pid            守护进程 pid(防重复启动)
    agent_cli_child.pid       agent_cli 子进程 pid(给 run_loop.sh stop 兜底清理用)
"""
from __future__ import annotations

import argparse
import ctypes
import errno
import json
import os
import platform
import shlex
import shutil
import signal
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from datetime import datetime

# 所有路径都以 work 目录(本脚本所在目录)为基准, 用相对写法 ——
# 宿主机 /home/nas/xiongqiumin/docker/work 与容器内 /home/nas/work 都能直接跑。
# ⚠ 分隔符一律用 '/': 这条相对路径要经过 shlex 拆词(见 split_cmd), 反斜杠会被当转义符吃掉
# ⚠ 构建产物位置: 整个 binary/ 下的 exe 统一在 <build>/binary/bin/<配置>/(见 binary/CMakeLists.txt),
#   单配置生成器(Linux)没有配置子目录 ⇒ build/binary/bin/agent_cli。
AGENT_BIN_REL = "JZFramework/build/binary/bin/agent_cli"
WORK_DIR = os.path.dirname(os.path.abspath(__file__))
DEFAULT_CWD = WORK_DIR                                     # agent_cli 的工作目录 = work 目录
PROMPT_FILE = os.path.join(WORK_DIR, "prompt.txt")

DEFAULT_PROMPT = (
    "请持续优化 JZFramework 的代码: 以 docs/ 下的设计文档为唯一目标来源, "
    "按优先级挑一个\"文档里写了但代码还没实现 / 实现不完整 / 与文档不一致\"的点。"
    "每轮只做一个可验证的小改动: 先读相关文档与现有代码, 再改实现, "
    "然后编译并跑相关测试, 通过后再继续下一个。"
    "不要修改 docs/ 里的目标本身, 也不要为了让测试通过而弱化测试。"
)

DEFAULT_HTTP_URL = "http://127.0.0.1:8080"   # config.json 里读不到时的兜底(agent_cli 的默认端口)
DEFAULT_MAX_LOG_MB = 20.0
DEFAULT_RETRY_SEC = 300.0        # agent_cli 退出后等多久再拉起
DEFAULT_RECHECK_SEC = 300.0      # 外部复验命令的运行上限
DEFAULT_ROUND_TIMEOUT_SEC = 7200.0   # 单轮 POST /task 的等待上限(模型慢, 给宽点)
DEFAULT_READY_SEC = 120.0        # 等 HTTP 就绪的上限
DEFAULT_STOP_TAG = "[loop-stop]" # 验证程序输出里出现它 ⇒ 不再提交下一轮(约定, 见 primes_verify.py)
CHUNK = 65536
PR_SET_PDEATHSIG = 1
# Windows 没有 SIGKILL(只有 SIGTERM), 那边用 taskkill /F 强制
SIG_FORCE = getattr(signal, "SIGKILL", signal.SIGTERM)

# 提前 dlopen + dlsym, 这样 preexec_fn 里只做一次系统调用
try:
    _LIBC = ctypes.CDLL("libc.so.6", use_errno=True)
    _LIBC.prctl                                          # noqa: B018
except Exception:                                        # 非 glibc / 非 Linux
    _LIBC = None


def _preexec_tie_to_parent() -> None:
    """在 fork 之后、exec 之前执行: 让 agent_cli 与 agent_loop.py 同生共死。

    父进程(agent_loop.py)一旦消失 —— 包括被 kill -9、被 OOM、崩溃 ——
    内核立刻给子进程发 SIGKILL, 不依赖我们的信号处理器是否来得及跑。
    """
    if _LIBC is None:
        return
    try:
        _LIBC.prctl(PR_SET_PDEATHSIG, signal.SIGKILL, 0, 0, 0)
    except Exception:
        return
    # 竞态: prctl 之前父进程可能就已经没了(此时 ppid 会被 init 收养)
    if os.getppid() == 1:
        os._exit(1)


def ts() -> str:
    return datetime.now().strftime("%Y-%m-%d %H:%M:%S")


def read_pid(path: str):
    """pidfile 第一行是 pid, 第二行是宿主机名(用于区分容器/宿主机两个 PID 命名空间)。"""
    try:
        with open(path, "r", encoding="utf-8") as f:
            return int((f.readline() or "").strip() or 0) or None
    except Exception:
        return None


def pid_alive(pid) -> bool:
    """进程还在吗?

    ⚠ Windows 上 os.kill(pid, 0) 靠不住: pid 不存在时抛的是 OSError [WinError 87]
    (不是 ProcessLookupError), 而"刚退出的 pid"又常被误判成存在 —— 拿它当依据去
    TerminateProcess, 轻则白等一轮, 重则误伤复用了这个 pid 的别的进程。
    所以那边改成 OpenProcess + GetExitCodeProcess(STILL_ACTIVE)。
    """
    if not pid:
        return False
    if os.name == "nt":
        return _win_pid_alive(pid)
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    except OSError:
        return False
    return True


def _win_pid_alive(pid) -> bool:
    import ctypes
    from ctypes import wintypes
    PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
    STILL_ACTIVE = 259
    k32 = ctypes.windll.kernel32
    h = k32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, int(pid))
    if not h:
        return False
    try:
        code = wintypes.DWORD()
        if not k32.GetExitCodeProcess(h, ctypes.byref(code)):
            return False
        return code.value == STILL_ACTIVE
    finally:
        k32.CloseHandle(h)


def _win_taskkill(pid) -> None:
    """Windows: taskkill /F /T 连子孙一起收(那边没有进程组, 也没有 SIGKILL)。

    ⚠ 不带 /F 的 taskkill 对控制台程序**没用**: 它只是"发个终止信号", 实测进程照样活着
    (而且它还会回 SUCCESS, 看着像成功)。所以这边只能强杀 —— 真正温和的收尾得靠程序自己
    响应 Ctrl+C, 不是这里的事。
    """
    cmd = ["taskkill", "/F", "/T", "/PID", str(int(pid))]
    try:
        subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=15)
    except Exception:                                        # noqa: BLE001
        pass


def _signal(pid, sig) -> None:
    """发信号停进程; 子进程是组长时连整个进程组一起(agent_cli 派生的子进程也能收掉)。

    ⚠ Windows 没有进程组 / os.getpgid / os.killpg / SIGKILL, 且 taskkill 不带 /F 不生效 ——
    那边一律强杀整棵树。⚠ SIGKILL 在 Windows 上根本不存在(SIG_FORCE 会退化成 SIGTERM),
    所以**不要**用 "sig != SIGTERM" 之类的方式判断要不要强制: 那边的分支写死强杀。
    """
    if os.name == "nt":
        _win_taskkill(pid)
        return
    try:
        if os.getpgid(pid) == pid:
            os.killpg(pid, sig)
        else:
            os.kill(pid, sig)
    except OSError:
        pass


def kill_pid(pid, sup: "SupervisorLog | None", what: str, wait: float = 5.0) -> None:
    """停掉进程(连子孙)。POSIX: 先 SIGTERM, 超时再 SIGKILL; Windows: 一步强杀 + 确认。

    pid 不在就静默返回。⚠ 判"还在不在"用 pid_alive(Windows 走 OpenProcess, 见那里的说明):
    已 wait 过的 pid 在 Windows 上会被 os.kill(pid,0) 误判成存活。
    """
    if not pid or not pid_alive(pid):
        return
    if os.name == "nt":
        if sup:
            sup.write(f"KILL {what} pid={pid}")
        _win_taskkill(pid)
        end = time.time() + wait
        while time.time() < end:
            if not pid_alive(pid):
                return
            time.sleep(0.2)
        if sup:
            sup.write(f"WARN {what} pid={pid} 强杀后仍在, 再补一次")
        _win_taskkill(pid)
        return

    if sup:
        sup.write(f"KILL {what} pid={pid} (SIGTERM)")
    _signal(pid, signal.SIGTERM)
    end = time.time() + wait
    while time.time() < end:
        if not pid_alive(pid):
            return
        time.sleep(0.2)
    if pid_alive(pid):
        if sup:
            sup.write(f"KILL {what} pid={pid} (SIGKILL, SIGTERM 超时)")
        _signal(pid, signal.SIGKILL)


def popen_kwargs() -> dict:
    """Popen 的平台相关参数。

    ⚠ Windows: preexec_fn 会直接 ValueError(没有 fork), start_new_session 被静默忽略 ——
    子进程与守护同生共死/整组收掉在那边只能靠 taskkill /T 与 JZProcess 自己的作业对象。
    """
    if os.name == "nt":
        return {}
    return {"preexec_fn": _preexec_tie_to_parent,   # 父进程一死, 子进程立刻被 SIGKILL
            "start_new_session": True}             # 自成进程组, 便于整组收掉


def hostname() -> str:
    """本机名(pidfile 第 2 行, 供 run_loop.sh 区分容器/宿主机)。

    ⚠ os.uname() 在 Windows 上根本不存在(AttributeError) —— 那边用 platform.node()。
    """
    try:
        return os.uname().nodename
    except AttributeError:
        return platform.node()


# ---------------------------------------------------------------- 提示词 / 提交信息
def load_prompt(path: str = "") -> str:
    """提示词优先取 --prompt-file(默认脚本同目录的 prompt.txt, 改提示词不用动代码), 没有就用内置默认。"""
    p = path or PROMPT_FILE
    try:
        with open(p, "r", encoding="utf-8") as f:
            txt = f.read().strip()
        if txt:
            return txt
    except OSError:
        pass
    return DEFAULT_PROMPT


def rel_to_work(path: str) -> str:
    """显示用: 在 work 目录下的就换成相对写法(跨宿主机/容器都成立)。"""
    try:
        rel = os.path.relpath(path, WORK_DIR)
    except ValueError:
        return path
    return path if rel.startswith("..") else rel


def split_cmd(text: str):
    """按 shell 规则拆一条命令(尊重引号)。

    ⚠ 反斜杠: posix=True 会把它当转义符 —— Windows 的 "C:\\a\\b.exe" 会被吃成 "C:ab.exe"
    (踩过: 默认的 agent_cli 相对路径就这么被拆没了)。所以 Windows 上用 posix=False 再手工
    去引号(它不认转义, 只按引号切); POSIX 那边保持不变。约定仍是: 路径尽量写 '/'。
    """
    if os.name != "nt":
        return shlex.split(text)
    toks = shlex.split(text, posix=False)
    out = []
    for t in toks:
        if len(t) >= 2 and t[0] == t[-1] and t[0] in "\"'":
            t = t[1:-1]
        out.append(t)
    return out


def read_cli_http(agent_argv) -> dict:
    """从 agent_cli 的配置里读出 HTTP 监听信息(URL + token), 读不到就返回 {}。

    为什么不让用户手填 URL: 端口本来就在 config.json 里(部署时在那儿改), 再让本脚本抄一份
    必然两边对不上。取值顺序: 命令行的 -c/--config -> exe 同目录的 config.json。
    """
    path = ""
    for i, tok in enumerate(agent_argv):
        if tok in ("-c", "--config") and i + 1 < len(agent_argv):
            path = agent_argv[i + 1]
        elif tok.startswith("--config="):
            path = tok.split("=", 1)[1]
    if not path and agent_argv:
        exe_dir = os.path.dirname(os.path.abspath(agent_argv[0]))
        path = os.path.join(exe_dir, "config.json")
    try:
        with open(path, "r", encoding="utf-8") as f:
            cfg = json.load(f)
    except Exception:                                        # noqa: BLE001
        return {}
    if not isinstance(cfg, dict):
        return {}
    http = cfg.get("http") if isinstance(cfg.get("http"), dict) else {}
    bind = http.get("bind") or cfg.get("httpBind") or "127.0.0.1"
    port = http.get("port") or cfg.get("httpPort") or 8080
    token = http.get("token") or cfg.get("httpToken") or ""
    return {"url": "http://%s:%s" % (bind, port), "token": token}


def clip(text: str, limit: int) -> str:
    """单行日志用的截断(折掉换行)。"""
    t = " ".join(text.split())
    return t if len(t) <= limit else t[:limit] + "…"


def last_line(text: str) -> str:
    """最后一个非空行(验证输出里最要紧的通常是最后那行结论)。"""
    lines = [ln.strip() for ln in (text or "").splitlines() if ln.strip()]
    return lines[-1] if lines else ""


_LINE_BUF_PREFIX = None


def line_buffer_prefix():
    """C/C++ 程序往管道写时 stdout 是全缓冲的, 日志会长时间空白 —— 有 stdbuf 就强制行缓冲。

    只探测一次并缓存; 没有 stdbuf / LD_PRELOAD 不可用(容器里偶发)就返回空列表, 不影响启动。
    """
    global _LINE_BUF_PREFIX
    if _LINE_BUF_PREFIX is not None:
        return _LINE_BUF_PREFIX
    _LINE_BUF_PREFIX = []
    if os.environ.get("LINE_BUFFER", "1") == "0":
        return _LINE_BUF_PREFIX
    stdbuf = shutil.which("stdbuf")
    if not stdbuf:
        return _LINE_BUF_PREFIX
    try:
        r = subprocess.run([stdbuf, "-oL", "/bin/echo", "ok"],
                           capture_output=True, timeout=5)
        if r.returncode == 0 and b"ok" in r.stdout:
            _LINE_BUF_PREFIX = [stdbuf, "-oL", "-eL"]
    except Exception:                                        # noqa: BLE001
        pass
    return _LINE_BUF_PREFIX


class RotatingLog:
    """按"天"切换 + 单文件字节上限切分。写入方只有 pump 线程, 但仍加锁以防将来多写。"""

    def __init__(self, log_dir: str, prefix: str, max_bytes: int, sup: "SupervisorLog"):
        self.dir = log_dir
        self.prefix = prefix
        self.max_bytes = max(1024, int(max_bytes))
        self.sup = sup
        self.lock = threading.Lock()
        self.fh = None
        self.path = ""
        self.size = 0
        self.day = ""

    # ---------- 文件名 ----------
    def _name(self, day: str, seq: int) -> str:
        base = f"{self.prefix}_{day}"
        return os.path.join(self.dir, f"{base}.log" if seq == 1 else f"{base}_{seq}.log")

    def _open_for(self, now: datetime, first: bool) -> None:
        """打开 today 当天"第一个还没写满"的文件(没有就新建), 并记录切换。"""
        day = now.strftime("%Y%m%d")
        seq = 1
        while True:
            path = self._name(day, seq)
            if not os.path.exists(path):
                size, mode = 0, "wb"
                break
            size = os.path.getsize(path)
            if size < self.max_bytes:
                mode = "ab"
                break
            seq += 1

        old = self.path
        if self.fh is not None:
            try:
                self.fh.close()
            except Exception:                                # noqa: BLE001
                pass
        self.fh = open(path, mode, buffering=0)
        self.path, self.size, self.day = path, size, day
        if first:
            self.sup.write(f"LOG {path}{' (续写)' if size else ''}")
        elif path != old:
            self.sup.write(f"ROTATE {old} -> {path} (上限 {self.max_bytes} B)")

    # ---------- 写入 ----------
    def write(self, data: bytes) -> None:
        if not data:
            return
        with self.lock:
            now = datetime.now()
            if self.fh is None:
                self._open_for(now, first=True)
            if self.day != now.strftime("%Y%m%d"):
                self._open_for(now, first=False)     # 跨天 -> 换新文件
            # 按剩余空间切片, 保证单个文件绝不超过上限
            while data:
                room = self.max_bytes - self.size
                if room <= 0:
                    self._open_for(now, first=False)  # 写满 -> 换下一个文件
                    room = self.max_bytes - self.size
                piece, data = data[:room], data[room:]
                self.fh.write(piece)
                self.size += len(piece)

    def close(self) -> None:
        with self.lock:
            if self.fh is not None:
                try:
                    self.fh.close()
                except Exception:                            # noqa: BLE001
                    pass
                self.fh = None


class SupervisorLog:
    """一行一条生命周期记录, 方便 grep 统计重启次数/轮次数。"""

    def __init__(self, path: str):
        self.path = path
        self.lock = threading.Lock()

    def write(self, msg: str) -> None:
        line = f"{ts()} pid={os.getpid()} {msg}\n"
        with self.lock:
            with open(self.path, "a", encoding="utf-8") as f:
                f.write(line)
        try:
            sys.stdout.write("[supervisor] " + line)
            sys.stdout.flush()
        except Exception:                                    # noqa: BLE001
            pass


class Runner:
    def __init__(self, args, rot: RotatingLog, sup: SupervisorLog):
        self.args = args
        self.rot = rot
        self.sup = sup
        self.stop = threading.Event()
        self.child = None
        self._child_pid = None          # 子进程 pid, 用于"无论如何"都要收掉它
        self.child_pid_file = os.path.join(args.log_dir, "agent_cli_child.pid")
        self.rounds = 0                 # 已提交的轮数
        self.recheck_fails = 0          # "验证程序说停但复验不过"的次数
        self._killed_by_us = False      # 这个子进程是我们主动收掉的(不是自己退的)
        self._child_start_ms = 0.0

    # ---------- 信号 ----------
    def install_signals(self) -> None:
        for sig in (signal.SIGINT, signal.SIGTERM):
            signal.signal(sig, self._on_stop)
        # 后台常驻: 明确忽略 SIGHUP, 终端关闭时不要跟着死
        try:
            signal.signal(signal.SIGHUP, signal.SIG_IGN)
        except (AttributeError, ValueError):
            pass

    def _on_stop(self, signum, _frame):
        self.sup.write(f"SIGNAL 收到 {signal.Signals(signum).name}, 准备退出")
        self.stop.set()
        # 只发信号不等待(信号处理器里不做阻塞操作), 收尾交给主循环的 finally
        if self._child_pid and pid_alive(self._child_pid):
            _signal(self._child_pid, signal.SIGTERM)

    # ---------- 子进程 ----------
    def child_running(self) -> bool:
        return self.child is not None and self.child.poll() is None

    def _pump(self, fd: int) -> None:
        try:
            while True:
                try:
                    chunk = os.read(fd, CHUNK)
                except OSError as e:
                    if e.errno in (errno.EIO, errno.EBADF):
                        break
                    raise
                if not chunk:
                    break
                self.rot.write(chunk)
        except Exception as e:  # noqa: BLE001
            self.sup.write(f"WARN 读取子进程输出失败: {e!r}")

    def start_child(self) -> bool:
        """拉起 agent_cli; 起来就返回 True(就绪与否另说)。"""
        cmd = self.args.command
        spawn = line_buffer_prefix() + list(cmd)
        env = dict(os.environ)
        env.setdefault("PYTHONUNBUFFERED", "1")   # 避免子进程块缓冲导致日志迟迟不落盘
        self.sup.write(f"START cwd={self.args.cwd} 命令=" + " ".join(shlex.quote(c) for c in cmd)
                       + ("  [已套 stdbuf 行缓冲]" if len(spawn) != len(cmd) else ""))
        self._child_start_ms = time.time()
        try:
            self.child = subprocess.Popen(
                spawn, cwd=self.args.cwd, env=env,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                stdin=subprocess.DEVNULL, bufsize=0,
                **popen_kwargs()
            )
        except FileNotFoundError:
            self.sup.write(f"ERROR 找不到可执行文件: {cmd[0]!r} (PATH 里有吗? 容器内才有 agent_cli?)")
            self.child = None
            return False
        except Exception as e:  # noqa: BLE001
            self.sup.write(f"ERROR 启动子进程失败: {e!r}")
            self.child = None
            return False

        self._child_pid = self.child.pid
        try:
            with open(self.child_pid_file, "w", encoding="utf-8") as f:
                f.write(str(self.child.pid))
        except Exception:                                    # noqa: BLE001
            pass
        t = threading.Thread(target=self._pump, args=(self.child.stdout.fileno(),),
                             daemon=True, name="pump")
        t.start()
        self._pump_thread = t
        return True

    def child_exit_line(self) -> None:
        """子进程已经退出: 记一行(退出码/信号 + 运行时长), 清 pidfile。"""
        if self.child is None:
            return
        try:
            self.child.wait(timeout=10)
        except Exception:                                    # noqa: BLE001
            pass
        dur = time.time() - getattr(self, "_child_start_ms", time.time())
        rc = self.child.returncode
        where = (" log=" + self.rot.path) if self.rot.path else ""
        if rc is not None and rc < 0:
            try:
                signame = signal.Signals(-rc).name
            except Exception:                                # noqa: BLE001
                signame = str(-rc)
            self.sup.write(f"EXIT signal={signame} rc={rc} runtime={dur:.1f}s{where}")
        else:
            self.sup.write(f"EXIT rc={rc} runtime={dur:.1f}s{where}")
        if rc is None:
            self.sup.write("WARN 子进程没在 10s 内退出(强杀也没效?), 先不管它继续")
        elif dur < 60 and not self._killed_by_us:
            # 我们自己收工杀的不算"异常早退"(那边退出码本来就是 1)
            self.sup.write(f"WARN agent_cli 只跑了 {dur:.1f}s 就退出, 检查一下日志是不是启动参数有问题")
        self._killed_by_us = False

        # 兜底: 还活着的(还没退干净的)一律收掉
        if self.child_running():
            kill_pid(self._child_pid, self.sup, "agent_cli(残留)", wait=5.0)
        # ⚠ 只有进程真退了才能关 stdout: pump 线程还阻塞在 os.read 上时关它会把本进程卡死
        #   (实测: 子进程杀不掉 -> 关闭管道 -> 整个驱动 hang)。还活着就先不关, 交给进程退出回收。
        if not self.child_running():
            try:
                self.child.stdout.close()
            except Exception:                                # noqa: BLE001
                pass
        self.child = None
        self._child_pid = None
        try:
            os.unlink(self.child_pid_file)
        except OSError:
            pass

    def stop_child(self, why: str) -> None:
        """主动收掉子进程(收工 / 换一个)。"""
        if not self.child_running():
            self.child_exit_line()
            return
        self._killed_by_us = True
        kill_pid(self._child_pid, self.sup, f"agent_cli({why})", wait=5.0)
        self.child_exit_line()

    # ---------- HTTP ----------
    def _headers(self) -> dict:
        h = {"Content-Type": "application/json; charset=utf-8"}
        if self.args.http_token:
            h["Authorization"] = "Bearer " + self.args.http_token
        return h

    def _url(self, path: str) -> str:
        return self.args.http_url.rstrip("/") + path

    def wait_ready(self) -> bool:
        """等 HTTP 就绪(GET /info 能通)。子进程中途死了就返回 False。"""
        deadline = time.time() + self.args.ready_sec
        while time.time() < deadline and not self.stop.is_set():
            if not self.child_running():
                return False
            try:
                req = urllib.request.Request(self._url("/info"), headers=self._headers())
                with urllib.request.urlopen(req, timeout=5) as r:
                    if 200 <= r.status < 300:
                        return True
            except urllib.error.HTTPError as e:
                if e.code == 401:
                    self.sup.write("ERROR /info 返回 401: token 不对(--http-token / config.json 的 http.token)")
                    return False
                # 其他 4xx/5xx: 服务已经起来了, 只是这个请求不被接受
                try:
                    e.read()
                except Exception:                            # noqa: BLE001
                    pass
                return True
            except Exception:                                # noqa: BLE001
                pass
            self.stop.wait(0.5)
        return False

    def submit_round(self) -> dict:
        """提交一轮: POST /task, 阻塞到这一轮结束(成功/失败/取消)才回, 回包即结果。

        返回 None = 子进程那边已经不可用了(连接被拒/被打断) -> 交给外层重拉。
        """
        body = {"prompt": self.args.prompt}
        if self.args.verify_cmd:
            body["verify_cmd"] = self.args.verify_cmd
        data = json.dumps(body, ensure_ascii=False).encode("utf-8")
        req = urllib.request.Request(self._url("/task"), data=data, headers=self._headers())
        try:
            with urllib.request.urlopen(req, timeout=self.args.round_timeout) as r:
                return json.loads(r.read().decode("utf-8", "replace") or "{}")
        except urllib.error.HTTPError as e:
            # 4xx/5xx 也带 JSON body(忙 / token 不对 / prompt 为空 ...)
            raw = b""
            try:
                raw = e.read()
            except Exception:                                # noqa: BLE001
                pass
            text = raw.decode("utf-8", "replace").strip()
            if text:
                try:
                    return json.loads(text)
                except Exception:                            # noqa: BLE001
                    pass
            return {"status": "error", "error": "HTTP %d %s" % (e.code, text[:200])}
        except Exception as e:  # noqa: BLE001
            self.sup.write(f"WARN 第 {self.rounds} 轮提交中断: {e!r}")
            return None

    def recheck(self) -> int:
        """跑 --recheck-cmd: 在**验证程序留下的状态**上再判一次, 结果落 supervisor.log。

        为什么这一步有意义: "通过、备份、收工"都是验证程序自己做的, 守护这边没法确认那些
        留下的东西; 退出后再跑一遍同一个验证程序(带只判定不落盘的开关), 才是"外部复验"。
        """
        a = self.args
        try:
            cmd = split_cmd(a.recheck_cmd)
        except ValueError as e:
            self.sup.write(f"WARN 复验命令解析失败: {e}")
            return -1
        if not cmd:
            return 0
        self.sup.write("RECHECK " + " ".join(shlex.quote(c) for c in cmd))
        try:
            r = subprocess.run(cmd, cwd=a.cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               timeout=DEFAULT_RECHECK_SEC)
            rc = r.returncode
            out = r.stdout.decode("utf-8", "replace")
        except subprocess.TimeoutExpired:
            self.sup.write(f"RECHECK FAIL 复验超时({DEFAULT_RECHECK_SEC:.0f}s)")
            return -1
        except Exception as e:  # noqa: BLE001
            self.sup.write(f"RECHECK FAIL 复验起不来: {e!r}")
            return -1
        for line in [ln for ln in out.splitlines() if ln.strip()][:20]:
            self.sup.write("RECHECK| " + line.strip())
        self.sup.write(f"RECHECK {'OK' if rc == 0 else 'FAIL'} rc={rc}")
        return rc

    # ---------- 主循环 ----------
    def run(self) -> int:
        a = self.args
        self.sup.write(f"LOOP 启动: cwd={a.cwd} url={a.http_url} log-dir={a.log_dir} "
                       f"max-log={self.rot.max_bytes}B retry={a.retry_sec:.0f}s "
                       f"round-timeout={a.round_timeout:.0f}s"
                       + (f" max-rounds={a.max_rounds}" if a.max_rounds > 0 else ""))
        if a.verify_cmd:
            self.sup.write("VERIFY-CMD " + a.verify_cmd)
        if a.recheck_cmd:
            self.sup.write("RECHECK-CMD " + a.recheck_cmd)

        while not self.stop.is_set():
            # ---- 1) 保证 agent_cli 在跑(HTTP 就绪) ----
            if not self.child_running():
                if not self.start_child():
                    if not self._sleep(a.retry_sec):
                        break
                    continue
                if not self.wait_ready():
                    if self.stop.is_set():
                        break
                    self.sup.write(f"WARN agent_cli 起来了但 HTTP 没就绪({a.http_url}), 收掉重来")
                    self.stop_child("就绪失败")
                    if not self._sleep(a.retry_sec):
                        break
                    continue
                self.sup.write(f"READY {a.http_url}")

            # ---- 2) 一轮接一轮提交 ----
            exited = False
            while not self.stop.is_set():
                if a.max_rounds > 0 and self.rounds >= a.max_rounds:
                    self.sup.write(f"STOP 已达 --max-rounds {a.max_rounds}, 收工(没等到停止标记)")
                    self.stop_child("达到轮数上限")
                    return 1

                self.rounds += 1
                t0 = time.time()
                self.sup.write(f"ROUND #{self.rounds} 提交 (prompt {len(a.prompt)} 字, "
                               f"verify={'有' if a.verify_cmd else '无'})")
                result = self.submit_round()
                el = time.time() - t0

                if result is None:
                    # 子进程不可用(崩了 / 端口没了): 交给外层重拉
                    self.stop_child("提交失败")
                    exited = True
                    break
                if "a task is already running" in str(result.get("error", "")):
                    # 上一轮还在跑(一般是上一次连接中途断了, 但服务端还在干活): 等一会再提
                    self.sup.write("WARN 服务端说还有一轮在跑(409), 等 10s 再提交")
                    self.rounds -= 1
                    if not self._sleep(10.0):
                        break
                    continue

                vout = result.get("last_verify_output", "") or ""
                status = result.get("status") or ("error" if result.get("error") else "?")
                self.sup.write(f"ROUND #{self.rounds} 结束 status={status} "
                               f"verify_rounds={result.get('verify_rounds', 0)} "
                               f"retries={result.get('retries', 0)} runtime={el:.1f}s "
                               f"verify={clip(last_line(vout), 200)}")
                if result.get("error"):
                    self.sup.write(f"WARN 第 {self.rounds} 轮 error: {clip(str(result.get('error')), 300)}")

                # 验证程序说"到此为止" -> 先外部复验, 再决定收工还是继续
                if a.stop_tag and a.stop_tag in vout:
                    self.sup.write(f"STOP 第 {self.rounds} 轮的验证输出里出现 {a.stop_tag}: 不再提交下一轮")
                    if a.recheck_cmd and self.recheck() != 0:
                        self.recheck_fails += 1
                        self.sup.write(f"WARN 外部复验没过 -> 视为没达成, 继续提交(第 {self.recheck_fails} 次)")
                        self.stop_child("复验未过")
                        if self.recheck_fails >= a.recheck_retries:
                            self.sup.write(f"ERROR 连续 {self.recheck_fails} 次\"要求收工但复验不过\", 收工退出")
                            return 1
                        exited = True
                        break
                    self.stop_child("收工")
                    self.sup.write("DONE 目标达成(验证程序要求停止 + 外部复验通过), 本次运行结束")
                    return 0

                if a.interval_sec > 0 and not self._sleep(a.interval_sec):
                    break

            # ---- 3) 子进程没了: 等一会重拉(循环回到顶部) ----
            if self.stop.is_set():
                break
            if not exited:
                # 内层是因为 stop/退出条件结束的, 走到这里说明 stop 了
                break
            if not self._sleep(a.retry_sec):
                break

        self.sup.write("LOOP 退出")
        return 0

    # ---------- 清理上一次异常退出留下的孤儿 ----------
    def kill_stale_child(self) -> None:
        stale = read_pid(self.child_pid_file)
        if pid_alive(stale):
            self.sup.write(f"WARN 发现上次遗留的 agent_cli pid={stale}, 先清理")
            kill_pid(stale, self.sup, "agent_cli(遗留)", wait=5.0)
        try:
            os.unlink(self.child_pid_file)
        except OSError:
            pass

    def _sleep(self, sec: float) -> bool:
        """可被打断的等待; 返回 False = 收到停止信号。"""
        end = time.time() + sec
        while not self.stop.is_set():
            left = end - time.time()
            if left <= 0:
                return True
            self.stop.wait(min(1.0, left))
        return False


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="agent_cli 无人值守驱动: 拉起 HTTP 服务 + 逐轮 POST /task + 收工复验 + 崩了重拉",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="示例:\n"
               "  python3 agent_loop.py --dry-run              # 先看会往哪个 URL 提交什么\n"
               "  python3 agent_loop.py --verify-cmd \"python test/primes_verify.py --backup-dir /tmp/b\"\n"
               "  python3 agent_loop.py --max-rounds 3         # 只跑 3 轮就收工(验证用)\n",
    )
    p.add_argument("--agent-cmd", default=os.environ.get("AGENT_CMD", ""),
                   help=f"要守护的 agent_cli 命令(默认: {AGENT_BIN_REL}, 相对 work 目录; "
                        f"要指定配置就写 \"... -c <config.json>\")")
    p.add_argument("--http-url", default=os.environ.get("AGENT_HTTP_URL", ""),
                   help=f"提交地址(默认: 从 agent_cli 的 config.json 读 http.bind/port, 读不到用 {DEFAULT_HTTP_URL})")
    p.add_argument("--http-token", default=os.environ.get("AGENT_HTTP_TOKEN", ""),
                   help="HTTP token(默认: 同来源 config.json 的 http.token; 空 = 不带)")
    p.add_argument("--prompt-file", default=os.environ.get("PROMPT_FILE", PROMPT_FILE),
                   help="提示词文件(默认: work 目录下的 prompt.txt; 不存在则用内置默认提示词)")
    p.add_argument("--verify-cmd", default=os.environ.get("VERIFY_CMD", ""),
                   help="每轮的验证命令(随每轮一起提交; 退出码 0 通过, 失败输出喂回模型续跑)")
    p.add_argument("--stop-tag", default=os.environ.get("STOP_TAG", DEFAULT_STOP_TAG),
                   help=f"验证输出里出现它就不再提交下一轮(默认 {DEFAULT_STOP_TAG}; 空 = 不认标记)")
    p.add_argument("--recheck-cmd", default=os.environ.get("RECHECK_CMD", ""),
                   help="外部复验命令: 见到停止标记后跑一次(建议复用同一个验证程序, 带只判定不落盘的开关), "
                        "通过才收工")
    p.add_argument("--recheck-retries", type=int, default=int(os.environ.get("RECHECK_RETRIES", 3)),
                   help="\"要求收工但复验不过\"最多容忍几次(默认 3, 之后退出码 1)")
    p.add_argument("--round-timeout", type=float,
                   default=float(os.environ.get("ROUND_TIMEOUT_SEC", DEFAULT_ROUND_TIMEOUT_SEC)),
                   help=f"单轮 POST /task 的等待上限秒数(默认 {DEFAULT_ROUND_TIMEOUT_SEC:g})")
    p.add_argument("--ready-sec", type=float,
                   default=float(os.environ.get("READY_SEC", DEFAULT_READY_SEC)),
                   help=f"等 agent_cli 的 HTTP 就绪的上限秒数(默认 {DEFAULT_READY_SEC:g})")
    p.add_argument("--interval-sec", type=float,
                   default=float(os.environ.get("INTERVAL_SEC", 0)),
                   help="两轮之间的等待秒数(默认 0 = 上一轮结束立刻提下一轮)")
    p.add_argument("--max-rounds", type=int, default=int(os.environ.get("MAX_ROUNDS", 0)),
                   help="本次运行最多提交几轮(默认 0 = 不限, 一直跑到验证程序要求停)")
    p.add_argument("--cwd", default=os.environ.get("AGENT_CWD", DEFAULT_CWD),
                   help="agent_cli 的工作目录(默认: work 目录本身)")
    p.add_argument("--log-dir", default=os.environ.get("LOG_DIR", os.path.join(WORK_DIR, "logs")),
                   help="日志目录(默认: work 目录下的 logs/)")
    p.add_argument("--max-log-mb", type=float,
                   default=float(os.environ.get("MAX_LOG_MB", DEFAULT_MAX_LOG_MB)),
                   help=f"单个日志文件上限 MB(默认 {DEFAULT_MAX_LOG_MB:g})")
    p.add_argument("--retry-sec", type=float,
                   default=float(os.environ.get("RETRY_SEC", DEFAULT_RETRY_SEC)),
                   help=f"agent_cli 退出后等待秒数(默认 {DEFAULT_RETRY_SEC:g} = 5 分钟)")
    p.add_argument("--log-prefix", default=os.environ.get("LOG_PREFIX", "agent_cli"),
                   help="日志文件名前缀(默认 agent_cli -> agent_cli_YYYYMMDD.log)")
    p.add_argument("--keep-days", type=int, default=int(os.environ.get("KEEP_DAYS", 0)),
                   help="只保留最近 N 天的日志, 0=不清理(默认)")
    p.add_argument("--dry-run", action="store_true", help="只打印配置(命令/URL/提示词来源), 不执行")
    p.add_argument("extra", nargs=argparse.REMAINDER,
                   help="'--' 之后的参数原样追加到 agent_cmd 后面")
    return p


def cleanup_old_logs(log_dir: str, prefix: str, keep_days: int, sup: SupervisorLog) -> None:
    if keep_days <= 0:
        return
    cutoff = time.time() - keep_days * 86400
    try:
        names = os.listdir(log_dir)
    except OSError:
        return
    for n in names:
        if not (n.startswith(prefix + "_") and n.endswith(".log")):
            continue
        path = os.path.join(log_dir, n)
        try:
            if os.path.getmtime(path) < cutoff:
                os.unlink(path)
                sup.write(f"PURGE 删除过期日志 {path}")
        except OSError:
            pass


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)

    extra = [x for x in args.extra if x != "--"]
    agent_cmd = args.agent_cmd or AGENT_BIN_REL
    try:
        cmd = split_cmd(agent_cmd) + extra
    except ValueError as e:
        print(f"[ERROR] --agent-cmd 解析失败: {e}", file=sys.stderr)
        return 2
    if not cmd:
        print("[ERROR] --agent-cmd 为空", file=sys.stderr)
        return 2
    args.command = cmd

    # HTTP 地址/token: 命令行优先, 否则从 agent_cli 的 config.json 读(读不到用默认)
    from_cfg = read_cli_http(cmd)
    args.http_url = args.http_url or from_cfg.get("url") or DEFAULT_HTTP_URL
    if not args.http_token:
        args.http_token = from_cfg.get("token") or ""
    args.prompt = load_prompt(args.prompt_file)

    pid_file = os.path.join(args.log_dir, "agent_loop.pid")
    max_bytes = int(args.max_log_mb * 1024 * 1024)

    if args.dry_run:
        rot = RotatingLog(args.log_dir, args.log_prefix, max_bytes, None)
        print("work 目录 :", WORK_DIR)
        print("命令      :", " ".join(shlex.quote(c) for c in cmd))
        print("工作目录  :", rel_to_work(args.cwd))
        print("提交地址  :", args.http_url + ("  (token 已配)" if args.http_token else "  (无 token)"))
        print("提示词来源:", "内置默认" if args.prompt == DEFAULT_PROMPT else rel_to_work(args.prompt_file))
        print("验证命令  :", args.verify_cmd or "(无: 每轮只驱动 agent 一次, 不验证)")
        print("停止标记  :", args.stop_tag or "(无: 一直提交到 --max-rounds / 被杀)")
        print("外部复验  :", args.recheck_cmd or "(无)")
        print("单轮上限  :", f"{args.round_timeout:g} s;  轮数上限:",
              f"{args.max_rounds} 轮" if args.max_rounds > 0 else "不限")
        print("日志目录  :", rel_to_work(args.log_dir))
        print("单个上限  :", f"{args.max_log_mb:g} MB ({max_bytes} B)")
        print("退出重试  :", f"{args.retry_sec:g} s")
        print("日志文件  :", rel_to_work(rot._name(datetime.now().strftime("%Y%m%d"), 1)))
        print("进程记录  :", rel_to_work(pid_file))
        return 0

    os.makedirs(args.log_dir, exist_ok=True)
    sup = SupervisorLog(os.path.join(args.log_dir, "supervisor.log"))
    rot = RotatingLog(args.log_dir, args.log_prefix, max_bytes, sup)

    # 防重复启动
    old = read_pid(pid_file)
    if pid_alive(old):
        print(f"[ERROR] agent_loop 已在运行 (pid={old}), 要重启请先 ./run_loop.sh stop", file=sys.stderr)
        return 3
    with open(pid_file, "w", encoding="utf-8") as f:
        f.write(f"{os.getpid()}\n{hostname()}\n")   # 第 2 行给 run_loop.sh 判环境

    if not os.path.isdir(args.cwd):
        sup.write(f"ERROR 工作目录不存在: {args.cwd}")
        os.unlink(pid_file)
        return 4

    cleanup_old_logs(args.log_dir, args.log_prefix, args.keep_days, sup)

    runner = Runner(args, rot, sup)
    runner.install_signals()
    runner.kill_stale_child()          # 收掉上次异常退出遗留的 agent_cli
    try:
        return runner.run()
    finally:
        # 无论怎么退出, 都保证 agent_cli 不残留(PDEATHSIG 之外的第二道保险)
        if runner._child_pid and pid_alive(runner._child_pid):
            kill_pid(runner._child_pid, sup, "agent_cli(退出兜底)", wait=5.0)
        rot.close()
        try:
            os.unlink(pid_file)
        except OSError:
            pass


if __name__ == "__main__":
    sys.exit(main())
