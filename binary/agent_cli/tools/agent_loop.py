#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""agent_cli 常驻守护: 拉起 -> 捕获输出(单文件 20MB / 每天切换) -> 记录退出 -> 等 5 分钟 -> 同样参数再拉起。

work 目录 = 本脚本所在目录(宿主机 ~/docker/work, 容器内 /home/nas/work);
路径一律用"相对 work 目录"的写法, 两个环境都能直接跑。

默认执行(可用 --agent-cmd / 环境变量 AGENT_CMD 覆盖):
    JZFramework/build/binary/agent_cli/agent_cli --loop "<prompt.txt 的内容>"

由 run_loop.sh 负责 {start|stop}; 也可以直接跑:
    python3 agent_loop.py --dry-run        # 只打印将要执行的命令/路径
    python3 agent_loop.py --once           # 只跑一轮, 退出后不再拉起(验证用)

只做【进程监控】: agent_cli 只要退出(正常退出 / 崩溃 / 被信号杀), 就记录退出码与运行时长,
等 retry-sec 秒再用同样参数拉起。不监视源码/二进制的变化 —— 要换代码就 ./run_loop.sh stop
再 start(或等 agent_cli 自己重编译后的下一次自然退出)。

生命周期与本进程绑定: agent_cli 带 PR_SET_PDEATHSIG(SIGKILL), 本进程一旦消失
(kill -9 / 崩溃 / 正常退出), 内核立刻杀掉它; 每轮结束与退出前另有兜底 SIGTERM。

产物(默认 <脚本目录>/logs, 可用 --log-dir 或环境变量 LOG_DIR 改):
    agent_cli_YYYYMMDD.log    agent_cli 原始输出(stdout+stderr 合并), 单文件上限 max-log-mb,
                              到上限自动续写 agent_cli_YYYYMMDD_2.log / _3.log ...; 跨天自动换新文件
    supervisor.log            每次 启动/退出/日志轮转/等待 的一行记录(含退出码与运行时长)
    agent_loop.pid            守护进程 pid(防重复启动)
    agent_cli_child.pid       agent_cli 子进程 pid(给 run_loop.sh stop 兜底清理用)
"""
from __future__ import annotations

import argparse
import ctypes
import errno
import os
import shlex
import shutil
import signal
import subprocess
import sys
import threading
import time
from datetime import datetime

# 所有路径都以 work 目录(本脚本所在目录)为基准, 用相对写法 ——
# 宿主机 /home/nas/xiongqiumin/docker/work 与容器内 /home/nas/work 都能直接跑。
WORK_DIR = os.path.dirname(os.path.abspath(__file__))
DEFAULT_CWD = WORK_DIR                                     # agent_cli 的工作目录 = work 目录
AGENT_BIN_REL = os.path.join("JZFramework", "build", "binary", "agent_cli", "agent_cli")
PROMPT_FILE = os.path.join(WORK_DIR, "prompt.txt")

DEFAULT_PROMPT = (
    "请持续优化 JZFramework 的代码: 以 docs/ 下的设计文档为唯一目标来源, "
    "按优先级挑一个\"文档里写了但代码还没实现 / 实现不完整 / 与文档不一致\"的点。"
    "每轮只做一个可验证的小改动: 先读相关文档与现有代码, 再改实现, "
    "然后编译并跑相关测试, 通过后再继续下一个。"
    "不要修改 docs/ 里的目标本身, 也不要为了让测试通过而弱化测试。"
)

DEFAULT_MAX_LOG_MB = 20.0
DEFAULT_RETRY_SEC = 300.0        # agent_cli 退出后等多久再拉起
CHUNK = 65536
PR_SET_PDEATHSIG = 1

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
    if not pid:
        return False
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def _signal(pid, sig) -> None:
    """子进程是组长时连整个进程组一起发(agent_cli 派生的子进程也能收掉)。"""
    try:
        if os.getpgid(pid) == pid:
            os.killpg(pid, sig)
        else:
            os.kill(pid, sig)
    except OSError:
        pass


def kill_pid(pid, sup: "SupervisorLog | None", what: str, wait: float = 5.0) -> None:
    """先 SIGTERM, 超时再 SIGKILL; pid 不在就静默返回。"""
    if not pid or not pid_alive(pid):
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


# ---------------------------------------------------------------- 提示词与命令
def load_prompt() -> str:
    """提示词优先取脚本同目录的 prompt.txt(改提示词不用动代码), 没有就用内置默认。"""
    try:
        with open(PROMPT_FILE, "r", encoding="utf-8") as f:
            txt = f.read().strip()
        if txt:
            return txt
    except OSError:
        pass
    return DEFAULT_PROMPT


def default_agent_cmd() -> str:
    """JZFramework/build/binary/agent_cli/agent_cli --loop "<提示词>"  (相对 work 目录)"""
    return " ".join(shlex.quote(x) for x in (AGENT_BIN_REL, "--loop", load_prompt()))


def rel_to_work(path: str) -> str:
    """显示用: 在 work 目录下的就换成相对写法(跨宿主机/容器都成立)。"""
    try:
        rel = os.path.relpath(path, WORK_DIR)
    except ValueError:
        return path
    return path if rel.startswith("..") else rel


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
    except Exception:                                    # noqa: BLE001
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

    def _open(self, path: str, mode: str, size: int) -> None:
        self.fh = open(path, mode, buffering=0)
        self.path = path
        self.size = size

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
            except Exception:
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

    def line(self, text: str) -> None:
        self.write((text + "\n").encode("utf-8", "replace"))

    def close(self) -> None:
        with self.lock:
            if self.fh is not None:
                try:
                    self.fh.close()
                except Exception:
                    pass
                self.fh = None


class SupervisorLog:
    """一行一条生命周期记录, 方便 grep 统计重启次数。"""

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
        except Exception:
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

    # ---------- 子进程输出泵 ----------
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

    # ---------- 主循环 ----------
    def run(self) -> int:
        a = self.args
        cmd = a.command                                    # 逻辑命令(实际执行的在前面套了行缓冲前缀)
        spawn = line_buffer_prefix() + list(cmd)           # 实际执行的(尽量带行缓冲)
        env = dict(os.environ)
        env.setdefault("PYTHONUNBUFFERED", "1")   # 避免子进程块缓冲导致日志迟迟不落盘

        self.sup.write(f"LOOP 启动: cwd={a.cwd} log-dir={a.log_dir} "
                       f"max-log={self.rot.max_bytes}B retry={a.retry_sec:.0f}s")
        self.sup.write("CMD " + " ".join(shlex.quote(c) for c in cmd)
                       + ("  [已套 stdbuf 行缓冲]" if len(spawn) != len(cmd) else ""))

        round_no = 0
        while not self.stop.is_set():
            round_no += 1
            started = time.time()
            self.sup.write(f"START #{round_no} cwd={a.cwd}")
            rc = None
            try:
                self.child = subprocess.Popen(
                    spawn, cwd=a.cwd, env=env,
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                    stdin=subprocess.DEVNULL, bufsize=0,
                    preexec_fn=_preexec_tie_to_parent,   # 父进程一死, 子进程立刻被 SIGKILL
                    start_new_session=True,              # 自成进程组, 便于整组收掉
                )
                self._child_pid = self.child.pid
                try:
                    with open(self.child_pid_file, "w", encoding="utf-8") as f:
                        f.write(str(self.child.pid))
                except Exception:
                    pass
                t = threading.Thread(target=self._pump, args=(self.child.stdout.fileno(),),
                                     daemon=True, name="pump")
                t.start()
                rc = self.child.wait()
                t.join(timeout=10)
                try:
                    self.child.stdout.close()
                except Exception:
                    pass
            except FileNotFoundError:
                rc = 127
                self.sup.write(f"ERROR 找不到可执行文件: {cmd[0]!r} (PATH 里有吗? 容器内才有 agent_cli?)")
            except Exception as e:  # noqa: BLE001
                rc = 126
                self.sup.write(f"ERROR 启动子进程失败: {e!r}")
            finally:
                # 兜底: 本轮的子进程若还活着(启动失败/被信号打断/跑完没退干净), 一律收掉
                if self._child_pid and pid_alive(self._child_pid):
                    kill_pid(self._child_pid, self.sup, "agent_cli(本轮兜底)", wait=5.0)
                self._child_pid = None
                self.child = None
                try:
                    os.unlink(self.child_pid_file)
                except OSError:
                    pass

            dur = time.time() - started
            raw_rc = rc if rc is not None else -1
            if raw_rc < 0:
                signame = signal.Signals(-raw_rc).name
                self.rot.line(f"\n===== [{ts()}] agent_cli 被信号终止 {signame} "
                              f"(returncode={raw_rc}) 运行 {dur:.0f}s =====")
                self.sup.write(f"EXIT #{round_no} signal={signame} rc={raw_rc} "
                               f"runtime={dur:.1f}s log={self.rot.path}")
            else:
                self.rot.line(f"\n===== [{ts()}] agent_cli 退出 returncode={raw_rc} "
                              f"运行 {dur:.0f}s =====")
                self.sup.write(f"EXIT #{round_no} rc={raw_rc} runtime={dur:.1f}s "
                               f"log={self.rot.path}")
            if dur < 60 and not self.stop.is_set():
                self.sup.write(f"WARN 第 {round_no} 轮只跑了 {dur:.1f}s 就退出, 检查一下日志是不是启动参数有问题")

            if self.stop.is_set() or a.once:
                break
            self.sup.write(f"SLEEP {a.retry_sec:.0f}s 后按同样参数重新拉起")
            self._sleep(a.retry_sec)

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

    def _sleep(self, sec: float) -> None:
        end = time.time() + sec
        while not self.stop.is_set():
            left = end - time.time()
            if left <= 0:
                return
            self.stop.wait(min(1.0, left))


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="agent_cli 常驻守护(日志 20MB/每日轮转, 退出后等 5 分钟再拉起)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="示例:\n"
               "  python3 agent_loop.py --dry-run              # 先看会执行什么\n"
               "  python3 agent_loop.py --once                 # 只跑一轮(前台验证)\n"
               "  python3 agent_loop.py --retry-sec 60         # 退出后等 60s 再拉起\n",
    )
    p.add_argument("--agent-cmd", default=os.environ.get("AGENT_CMD") or default_agent_cmd(),
                   help="要守护的完整命令, 会被 shell 规则拆词 "
                        f"(默认: {AGENT_BIN_REL} --loop \"<prompt.txt 的内容>\", 相对 work 目录)")
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
    p.add_argument("--once", action="store_true", help="只跑一轮, 退出后不重新拉起")
    p.add_argument("--dry-run", action="store_true", help="只打印配置, 不执行")
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
    try:
        cmd = shlex.split(args.agent_cmd) + extra
    except ValueError as e:
        print(f"[ERROR] --agent-cmd 解析失败: {e}", file=sys.stderr)
        return 2
    if not cmd:
        print("[ERROR] --agent-cmd 为空", file=sys.stderr)
        return 2
    args.command = cmd

    pid_file = os.path.join(args.log_dir, "agent_loop.pid")
    max_bytes = int(args.max_log_mb * 1024 * 1024)

    if args.dry_run:
        rot = RotatingLog(args.log_dir, args.log_prefix, max_bytes, None)
        print("work 目录 :", WORK_DIR)
        print("命令      :", " ".join(shlex.quote(c) for c in cmd))
        print("工作目录  :", rel_to_work(args.cwd))
        print("提示词来源:", "内置默认" if load_prompt() == DEFAULT_PROMPT else rel_to_work(PROMPT_FILE))
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
        f.write(f"{os.getpid()}\n{os.uname().nodename}\n")   # 第 2 行给 run_loop.sh 判环境

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
