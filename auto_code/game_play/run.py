#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
game_play 驱动 —— **一份 run.json 说了算**（题目 + 工作区 + 与题目无关的调参）

用法:
  python run.py <run.json>            # 唯一输入：一次运行的配置（不给就用本目录的 run.json）
  python run.py run.json --dry-run    # 只准备 + 编译 + 打印计划，不碰容器/agent
  python run.py run.json --no-agent   # 不起 agent，只跑局（调链路 / 复验脚本用）

`run.json` 里最要紧的两个字段就是**两个目录**（其余是与题目无关的调参）：
  · `testDir` = **题目（玩法）目录**（如 `auto_code/game_play/game/hello`）
  · `workDir` = **这一次运行的工作区**（如 `build/auto_code/game_play/hello`）
⚠ 配置里的相对路径一律按**仓库根**解析（不是 CWD）—— 于是同一份 run.json 在哪儿调用都指向同一处；
   绝对路径原样。示例见 `auto_code/game_play/run.json`。

两个目录的角色（**题目只读、工作区可写**）:
  · test_dir = **题目（玩法）目录**，自包含：`prompt.md` 任务书 / `config.json` 怎么跑怎么判 /
    `verify.py` 尺子 / 本体 `.ts`（+ 可选的参考脚本 `play.ts`）。驱动**只读**它。
  · work_dir = **这一次运行的工作区**（可写），什么都落这儿：
      play/play.ts         产出（agent 改的就是它；首次运行从"种子"拷进来）
      play/jzapi.d.ts      脚本能用的面（从任务根的模板拷）
      tsconfig.json        编 play（rootDir=工作区 ⇒ build/play/play.js）
      tsconfig.game.json   编题目本体（rootDir=题目目录 ⇒ build/game/<本体>.js）
      agent_cli.json       agent 配置（llm 来自配置；terminal.rootDir=工作区、题目目录进 trustedDirs）
      agent_system.md      **常驻说明**（环境简报 + 玩法任务书）→ agent_cli 启动时拼进 system prompt（只一次）
      plan.json            这次"会怎么做"（人看；--dry-run 只做这个 + 编译）
      build/               tsc 产物（容器按它装程序）
      runs/current/        本局证据：play.log / frames/ / external/ / metrics.jsonl / report.md /
                           result.json / session.json / Agent.log
      runs/round_NNN/      每轮归档；runs/scores.jsonl 一行一轮
      Agent.log            agent 回合诊断（think / tool / reply）
      driver.log           驱动自己的 stdout 副本（阶段 / 每轮小结 / **停止原因**）

`run.json` 里除两个目录以外的都是**与题目无关**的调参（llm / 轮次 / tsc / agent bin / stopWhen /
是否编译归档…），缺哪项就用内置默认；**题目侧的一切都从 `testDir` 推**（玩法名 = 目录名）。

会话怎么搭（2026-10-02 定）:
  · **常驻说明**（环境简报 + 玩法任务书）写进 `<work>/agent_system.md`，由 agent_cli 配置的
    `system.extraFile` 在**启动时拼进 system prompt 一次** —— 整场不再每轮重复贴。
  · 之后每轮 `POST /task` **只叠加增量**（这一轮的任务 + 上一局的证据）；agent_cli **不会**每轮清上下文
    （只有 `POST /reset` 会清，驱动从不发），所以它记得自己前几轮做过什么。

流程（每一轮，**每轮只有一次 LLM 回合**）:
  a) 让 agent 设计/改脚本   POST /task（这一轮的任务 + **上一局的证据**：尺子输出 / 归档 / 编译错误）
  b) 编译                   tsc：工作区(play) + 题目本体（都出到工作区 build/）
  c) 跑一局                 卸旧程序 → 装 game + play → 外面抓帧 → 等 session.json
  d) 判分 + 归档            题目自己的 verify.py（子进程）→ runs/round_NNN
  e) 出口                   轮次/时长上限 · 连续 stallRounds 轮没进展 · stopWhen=verify-pass
  ⚠ 曾经把 a) 拆成"设计轮(写) + 分析轮(看证据改)"两次 POST —— 信息重叠、墙钟翻倍（27B 每回合数分钟），
    2026-10-02 合并成一次：证据随 a) 一起给。副作用：不再认 `[script-ok]`（出口只看尺子判分/停滞/轮次）。

角色（别混）:
  · `work_dir/play/play.ts` —— **最终产出**（由 agent 迭代；本文件不碰它的内容）
  · `test_dir/**`           —— **题目侧**（不许改）
  · `test_dir/verify.py`    —— **尺子**（这里只是"加载"它：当子进程跑，读它的结论）
  · 本文件                  —— 驱动 / 编排 / 守夜（保证 agent 一直在跑 + 超时与停滞检测）
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from contextlib import ExitStack

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

HERE = os.path.dirname(os.path.abspath(__file__))          # 本文件所在 = 任务目录（env.py 在这）
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))     # 仓库根
sys.path.insert(0, HERE)
import env as E  # noqa: E402

DEFAULT_ROM = os.path.join("E:/work/simulator", "火焰之纹章-圣魔之光石完美修改版.gba")

# ---- 运行时才定下来的路径（main() 里 plan_paths() 赋；helper 函数直接用）----
TEST_DIR = ""            # 题目（玩法）目录
WORK_DIR = ""            # 本次运行的工作区
BUILD = ""               # <work>/build（tsc 产物）
RUNS = ""                # <work>/runs
CUR = ""                 # <work>/runs/current（本局证据）
LIVE = ""                # <work>/runs/current/live.json（给 watch.py 看的现场）
LIVE_MIRROR = ""         # <任务目录>/runs/current/live.json（watch.py 按自己的位置找那份）
HUMAN = ""               # <任务目录>/runs/current/human.json（watch.py 写在这）
HUMAN_WORK = ""          # <work>/runs/current/human.json（也读；两边都算数）
DRIVER_LOG = ""          # <work>/driver.log（驱动自己的 stdout 副本；start_driver_log() 里开）
# 常驻说明文件名（放在工作区根；agent_cli 的 system.extraFile 按 rootDir=工作区 解析它）：
# 内容 = 环境简报 + 玩法任务书 —— agent_cli 启动时读一次, 拼进 **system prompt**（不是每轮贴）
AGENT_SYSTEM_MD = "agent_system.md"


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


class _TeeStream:
    """stdout 双写：控制台照旧 + 追加进 `<work>/driver.log`（每次 write 都 flush）。

    ⚠ 为什么要这么做：驱动打印的**阶段与停止原因**（`== 尺子 PASS，按 stopWhen=verify-pass 收工`）
    原来只活在 stdout 上 —— 谁把驱动重定向/管到不落盘的地方（或被 IDE 吞掉），判断依据就没了，
    只能靠 live.json + scores.jsonl 反推。日志必须是**文件**。
    """

    def __init__(self, stream, path):
        self._stream = stream
        self._fh = open(path, "a", encoding="utf-8", errors="replace")

    def write(self, s):
        try:
            self._stream.write(s)
        except Exception:       # 控制台那头断了（管道关了/没接）不影响落盘
            pass
        try:
            self._fh.write(s)
            self._fh.flush()
        except Exception:
            pass
        return len(s)

    def flush(self):
        for x in (self._stream, self._fh):
            try:
                x.flush()
            except Exception:
                pass

    def close(self):
        self.flush()
        try:
            self._fh.close()
        except Exception:
            pass


def start_driver_log(argv):
    """开 `<work>/driver.log`，从这一刻起 stdout 双写（含每次运行的分隔头）。

    时机：`plan_paths()` 之后（知道工作区在哪了）立刻调 —— 于是本文件后面所有 print
    （计划、每轮小结、编译输出、agent 回包状态、停止原因）都同时进控制台与这个文件。
    """
    global DRIVER_LOG
    if DRIVER_LOG:
        return DRIVER_LOG
    os.makedirs(WORK_DIR, exist_ok=True)
    DRIVER_LOG = os.path.join(WORK_DIR, "driver.log")
    sys.stdout = _TeeStream(sys.stdout, DRIVER_LOG)
    print("\n===== %s  pid=%d  %s" % (time.strftime("%Y-%m-%d %H:%M:%S"), os.getpid(),
                                     " ".join(argv)))
    return DRIVER_LOG


def write_json(path, obj):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(obj, f, ensure_ascii=False, indent=2)
    return path


def read_text(path, limit=200000):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read(limit)
    except Exception:
        return ""


def rel_mix(path, base):
    """显示用：能给相对就给相对（跨目录时退化成绝对）。"""
    try:
        r = os.path.relpath(path, base)
        return path if r.startswith("..") else r
    except Exception:
        return path


def round_index():
    if not os.path.isdir(RUNS):
        return 0
    return len([d for d in os.listdir(RUNS) if d.startswith("round_")])


def resolve_from(value, base):
    """相对路径按 base 解析（位位参数按 CWD，配置里的 agent bin 等按任务目录）。"""
    if not value:
        return ""
    return os.path.normpath(value) if os.path.isabs(value) else os.path.normpath(os.path.join(base, value))


# ==================== 旁路观测 / 人类干预的口子（见 game/<玩法>/watch.py） ====================
def publish_live(ctx, phase, round_no=0, box=None, agent=None, extra=None):
    """把"现在跑到哪儿了"写进 `<work>/runs/current/live.json` —— 给旁路的 watch.py（人）看。

    ⚠ 同时往**任务目录**的 `runs/current/live.json` 镜像一份：watch.py 住在玩法目录里，
    它按"往上找到任务目录的那层"找现场（见 watch.py 的 TASK），所以两边都写最省事。
    每轮与阶段变化都覆盖写一次（临时文件 + 原子替换，读的人不会读到半截）。
    """
    j = {
        "ts": int(time.time()), "pid": os.getpid(), "phase": phase, "round": int(round_no),
        "play": ctx.get("play", ""), "app": ctx.get("app", ""), "program": ctx.get("program", ""),
        "window": ctx.get("window", ""), "native": list(ctx.get("native") or []),
        "desktop": ctx.get("desktop", ""), "status_path": ctx.get("status_path", ""),
        "container_url": (box.c.base if box is not None else "") or ctx.get("container_url", ""),
        "task_dir": HERE, "test_dir": TEST_DIR, "work_dir": WORK_DIR, "run_dir": CUR,
        "play_ts": ctx.get("play_ts", ""), "prompt_file": ctx.get("prompt_file", ""),
    }
    if agent is not None:
        j["agent_url"] = getattr(agent, "url", "")
        j["agent_token"] = getattr(agent, "token", "")
    if extra:
        j.update(extra)
    for path in (LIVE, LIVE_MIRROR):
        if not path:
            continue
        try:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            tmp = path + ".tmp"
            with open(tmp, "w", encoding="utf-8") as f:
                json.dump(j, f, ensure_ascii=False, indent=2)
            os.replace(tmp, path)
        except Exception:
            pass
    return j


def _read_human_file(path, limit):
    out = []
    try:
        with open(path, encoding="utf-8") as f:
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


def read_human(limit=3):
    """人在外面（watch.py）直接给 agent 下过的任务：取最近 limit 条（工作区/任务目录两边都读）。

    为什么要读它：agent 自己的会话里没有这段，但文件已经被那些人改过。不讲清楚，
    它下一轮会按"上一轮的认知"动手，跟文件现状打架。
    """
    recs = _read_human_file(HUMAN, limit) + _read_human_file(HUMAN_WORK, limit)
    recs.sort(key=lambda r: r.get("ts") or 0)
    return recs[-limit:]


def human_note():
    """把"人类插过手"写成一段给 agent 看的话（没有就返回空串）。"""
    hs = read_human()
    if not hs:
        return ""
    s = "\n【人类干预】注意：**有人在你之外也直接给这个 agent 下过任务**（他们看着同一块画面）：\n"
    for h in hs:
        s += "  · [%s] %s%s\n" % (h.get("ts_text", "?"), (h.get("prompt") or "(只发了画面)")[:200],
                                 "（附了截图）" if h.get("with_screen") else "")
    s += "  以**文件现状**为准：动手前先看你的产出脚本现在是什么样，别按你上一轮的印象改。\n"
    return s


def file_sig(path):
    """文件指纹（判"agent 这一轮到底改了没有"）。"""
    if not os.path.exists(path):
        return "(missing)"
    h = hashlib.sha1()
    with open(path, "rb") as f:
        h.update(f.read())
    return h.hexdigest()[:12]


# ==================== 工作区：脚手架 / 计划 ====================
# 任务级 tsconfig.json 是"编译口径"的单一来源（compilerOptions + 引擎类型那几个 include）；
# 工作区里的两份 tsconfig 由它派生（题目侧与产出各一份，各自的 rootDir/outDir 明确 ⇒ 产物路径可预期）。
DEFAULT_TYPES = [
    "JZJSEngine/js_wrapper/types/**/*.d.ts",
    "JZJSEngine/js_wrapper/widget/types/**/*.d.ts",
    "JZJSEngine/js_wrapper/fs/types/**/*.d.ts",
    "JZJSEngine/js_wrapper/path/types/**/*.d.ts",
    "JZJSEngine/js_wrapper/buffer/types/**/*.d.ts",
]

EMPTY_PLAY_TS = """// 产出脚本（工作区）—— **起点是空的**：只有"跟驱动打交道"的脚手架，没有任何玩法逻辑。
// 先把题目任务书（prompt.md）读一遍：规则 / 判分口径 / 红线 / 能用什么都在里面，然后在这里从零写。
//
// 这份骨架已经满足两条**硬契约**（别删）：
//   ① 按 `play.json` 的 steps / maxSeconds 收工，并在收工时写 `session.json`（没写 = 脚本卡死）；
//   ② 每个进程只靠 `__jz.computeruse.*` 看画面 / 操作键鼠，不读任何游戏内部状态（红线）。
import fs from 'fs';
import { Buffer } from 'buffer';

const cu = __jz.computeruse;
const dir = jzApp.applicationDirPath();
const cfg = JSON.parse(fs.readFileSync(dir + '/play.json', 'utf8'));
const t0 = Date.now();

function log(line) { console.log('[' + ((Date.now() - t0) / 1000).toFixed(1) + 's] ' + line); }
function sleep(ms) { if (ms > 0) cu.wait(ms); }
function shot(tag) {
    try {
        const data = cu.screenshot();
        const b64 = data.indexOf(',') >= 0 ? data.slice(data.indexOf(',') + 1) : data;
        fs.writeFileSync(cfg.out + '/frames/' + tag + '.png', Buffer.from(b64, 'base64'));
    } catch (e) { log('写帧失败: ' + e); }
}
function finish(reason, steps) {
    try {
        fs.writeFileSync(cfg.out + '/session.json',
            JSON.stringify({ script: 'play.ts', reason, steps, elapsedMs: Date.now() - t0 }, null, 2));
    } catch (e) { log('写 session.json 失败: ' + e); }
    log('[play] 收工: reason=' + reason + ' steps=' + steps);
}

function main() {
    log('[play] 目标窗口 ' + cfg.window + '，产物目录 ' + cfg.out);
    // TODO(agent): 等窗口 → 截图 → 决策 → click/key → 直到这个玩法判通关
    for (let step = 0; ; step++) {
        if (cfg.steps > 0 && step >= cfg.steps) return finish('steps', step);
        if (cfg.maxSeconds > 0 && (Date.now() - t0) / 1000 >= cfg.maxSeconds) return finish('time', step);
        if (step % 5 === 0) shot('f' + String(step).padStart(5, '0'));
        log('step ' + step + '（还没写策略）');
        sleep(cfg.stepMs);
    }
}
try { main(); } catch (e) { log('!! 脚本异常: ' + e); finish('crash', 0); }
"""


def task_tsconfig():
    """任务级 `tsconfig.json`（= **编译口径**）→ dict；坏了/没有就退回内置默认。

    只取两块：`compilerOptions`（原样拷进派生出的 tsconfig）与 `include` 里 `../` 开头的类型 glob。
    ⚠ 它**不是**一份能单独 `tsc -p` 的工程（没有指向源码的 include）；缺了它，派生的配置就只剩
      tsc 默认值（target ES5 / module CommonJS）⇒ 产物不是 ESM、容器加载不起来。
    """
    txt = read_text(os.path.join(HERE, "tsconfig.json"))
    try:
        return json.loads(re.sub(r"^\s*//.*$", "", txt, flags=re.M))
    except Exception:
        return {}


def _type_globs(task_ts, base):
    """引擎类型那几个 include（任务级 tsconfig 里以 `../` 开头的那几条），重锚到 base。"""
    types = [t for t in (task_ts.get("include") or []) if str(t).startswith("..")] or DEFAULT_TYPES
    out = []
    for t in types:
        p = os.path.normpath(os.path.join(HERE, t)) if str(t).startswith("..") else os.path.join(ROOT, t)
        out.append(os.path.relpath(p, base).replace("\\", "/"))
    return out


def prepare_work_dir(play, app_ts, dry=False):
    """把工作区搭起来（幂等：已存在的产出**不覆盖**；脚手架每次重写）。

    · `play/play.ts`    —— 首次才有：写一份**空骨架**（只有入口脚手架 + TODO，没有任何玩法逻辑）
    · `play/jzapi.d.ts` —— 从任务根模板拷（脚本能用的面）
    · `tsconfig.json` / `tsconfig.game.json` —— 派生自任务级 tsconfig
    返回 notes（给打印用）。
    """
    notes = []
    for sub in ("play", "build", "runs/current"):
        os.makedirs(os.path.join(WORK_DIR, *sub.split("/")), exist_ok=True)

    # --- 1) 脚本能用的面（模板在任务根）---
    tpl = os.path.join(HERE, "jzapi.d.ts")
    dst = os.path.join(WORK_DIR, "play", "jzapi.d.ts")
    if os.path.exists(tpl):
        shutil.copy2(tpl, dst)
    else:
        notes.append("!! 找不到类型模板 %s（脚本会缺 __jz 声明）" % tpl)

    # --- 2) 产出脚本：起点一律是**空的**（只在没有的时候铺一次；写出来的就是我们自己的，不覆盖）---
    play_ts = os.path.join(WORK_DIR, "play", "play.ts")
    if not os.path.exists(play_ts):
        with open(play_ts, "w", encoding="utf-8", newline="\n") as f:
            f.write(EMPTY_PLAY_TS)
        notes.append("产出脚本: 空骨架（首次生成，之后不覆盖）")
    else:
        notes.append("产出脚本: 已存在，不动它")

    # --- 3) 两份 tsconfig（产出 / 题目本体）---
    task_ts = task_tsconfig()
    if not (task_ts.get("compilerOptions") or {}):
        notes.append("!! 读不到任务级 tsconfig.json 的 compilerOptions —— 编译会退回 tsc 默认"
                     "（target ES5 / module CommonJS，容器加载不起来），先补上它")
    co_play = dict(task_ts.get("compilerOptions") or {})
    co_play.update({"outDir": "build", "rootDir": ".", "noEmitOnError": True, "sourceMap": False})
    write_json(os.path.join(WORK_DIR, "tsconfig.json"), {
        "compilerOptions": co_play,
        "include": ["play/**/*.ts", "play/**/*.d.ts"] + _type_globs(task_ts, WORK_DIR),
    })
    game_tsconfig = ""
    if app_ts:      # 本体住在别处（jsRoot=repo，如 shooter）时不给它生成
        co_game = dict(task_ts.get("compilerOptions") or {})
        co_game.update({
            "outDir": os.path.relpath(os.path.join(BUILD, "game"), WORK_DIR).replace("\\", "/"),
            "rootDir": os.path.relpath(TEST_DIR, WORK_DIR).replace("\\", "/"),
            "noEmitOnError": True, "sourceMap": False,
        })
        game_tsconfig = os.path.join(WORK_DIR, "tsconfig.game.json")
        write_json(game_tsconfig, {
            "compilerOptions": co_game,
            "files": [os.path.relpath(app_ts, WORK_DIR).replace("\\", "/")],
            "include": [_rel_glob(TEST_DIR, WORK_DIR)] + _type_globs(task_ts, WORK_DIR),
        })
    return game_tsconfig, notes


def _rel_glob(d, base):
    return os.path.relpath(os.path.join(d, "**", "*.d.ts"), base).replace("\\", "/")


# ==================== agent_cli 的配置 ====================
def agent_bin_path(S):
    """agent_cli.exe 在哪：配置的 `agent.bin` > 默认构建位置（`build/binary/bin/<cfg>/`）。

    `agent.bin` 写相对路径时按 **任务目录 → 仓库根** 依次找（老配置写的是"任务目录相对"，
    新的"仓库根相对"也认）；绝对路径原样。其它路径字段（tsc.path / dllDir / rom…）建议直接写绝对。
    """
    v = ((S.get("agent") or {}).get("bin") or S.get("agentBin") or "")
    cands = []
    if v:
        cands += [v] if os.path.isabs(v) else [resolve_from(v, HERE), resolve_from(v, ROOT)]
    cands.append(os.path.join(ROOT, "build", "binary", "bin", "Debug", "agent_cli.exe"))
    for c in cands:
        if os.path.exists(c):
            return c
    return cands[0]


def write_agent_config(S, bin_path, test_dir):
    """写 `<work>/agent_cli.json`（生成物，每次重写）。

    · llm   —— 配置里的 `llm`；没配就用 **agent_cli 自己那份** config.json 的 llm（换机器不用改这里）
    · 工作区 —— `terminal.rootDir` = 工作区（agent 在这里改产出）；`trustedDirs` += 题目目录（只读地看）
    · 日志 —— `logFile` 非空才有 `Agent.log`（回合诊断：think / tool / reply）落盘，归档也带上
    """
    llm = dict(S.get("llm") or {})
    if not llm.get("url"):
        exe_cfg = E.read_json(os.path.join(os.path.dirname(bin_path), "config.json"), {}) or {}
        llm = dict((exe_cfg.get("llm") if isinstance(exe_cfg.get("llm"), dict) else {}) or {})
    llm.setdefault("url", "")
    llm.setdefault("name", "my_model")
    llm.setdefault("netTimeoutMs", 120000)
    llm.setdefault("maxRounds", 100)
    term = dict((S.get("terminal") or {}))
    term.update({
        "rootDir": WORK_DIR,
        "trustedDirs": [test_dir] + list(term.get("trustedDirs") or []),
    })
    term.setdefault("allowTerminal", True)
    term.setdefault("allowFileTools", True)
    cfg = {
        "agentName": "agent_cli", "version": "1.0.0",
        "logFile": os.path.join(WORK_DIR, "agent.log").replace("\\", "/"),
        "llm": llm,
        "verify": dict(S.get("verifyCfg") or {"maxRounds": 30, "timeoutMs": 60000}),
        "memory": {"enabled": True, "maxBytes": 8192,
                   "file": os.path.join(WORK_DIR, "agent_memory.md").replace("\\", "/")},
        # 常驻说明(环境简报 + 玩法任务书): 由 write_agent_system() 写进 <work>/agent_system.md,
        # agent_cli 启动时把它拼进 **system prompt 一次** —— 之后每轮 /task 只叠加新消息。
        "system": {"extraFile": AGENT_SYSTEM_MD},
        "terminal": term,
        "http": {"bind": "127.0.0.1", "port": E.free_port(), "token": "", "dir": ""},
    }
    return write_json(os.path.join(WORK_DIR, "agent_cli.json"), cfg)


# ==================== 给 agent 的两段任务（"生成"与"分析"） ====================
def env_brief(ctx):
    return (
        "【环境】\n"
        "· 虚拟设备容器: %s（无头虚拟桌面；玩法 %s：进程名 %s、窗口标题 %s、客户区 %dx%d）\n"
        "· 任务书: %s（**先读它**：红线、产出物形状、观测面都在里面）\n"
        "· 产出物（你要改的就是它）: %s\n"
        "· 证据目录: %s（每轮被覆盖：play.log / frames/ / external/ / metrics.jsonl / report.md / session.json / result.json）\n"
        "· 你的工作区: %s（在这里改脚本；题目目录 %s **只读**）\n"
        "· 驱动会做: 编译你的脚本 → 在容器里跑一局 → 跑尺子 verify.py → 归档；你不用自己跑实验。\n"
        % (ctx["container_url"], ctx["play"], ctx["program"], ctx["window"], ctx["native"][0],
           ctx["native"][1], ctx["prompt_file"], ctx["play_ts"], CUR, WORK_DIR, TEST_DIR)
    )


def agent_system_path():
    return os.path.join(WORK_DIR, AGENT_SYSTEM_MD)


def write_agent_system(ctx, taskbook):
    """写**常驻说明** `<work>/agent_system.md` —— agent_cli 启动时把它拼进 **system prompt 一次**。

    内容 = 环境简报 + **玩法任务书原文**：这两样整场都不变 ⇒ 不该每轮重复贴（每轮只叠加
    "这一轮的任务 + 上一局的证据"，见 design_prompt）。agent_cli 侧见配置 `system.extraFile`。
    ⚠ 必须在 agent_cli 启动**之前**写好（它启动时读一次）；改内容得重拉 agent_cli。
    """
    text = ("本文件是**常驻说明**：下面这些从第 1 轮到最后一轮都有效，不会再每轮重复贴给你。\n\n"
            + env_brief(ctx)
            + "\n===== 玩法任务书原文（红线 / 判分口径 / 产出契约都在这里面）=====\n"
            + (taskbook or "(任务书为空)") + "\n")
    with open(agent_system_path(), "w", encoding="utf-8") as f:
        f.write(text)
    return agent_system_path()


def evidence_block(ctx, prev, human=""):
    """**上一局的证据**（拼进下一轮的设计任务里）。

    每轮只有一次 LLM 回合 ⇒ "跑完 → 看证据 → 改"必须在**同一个任务**里闭环：
    证据 = 尺子结论（退出码 + 输出尾部）+ 归档目录 + 证据目录（脚本自己看 play.log/report.md/metrics.jsonl）
    + 编译错误（如果上一轮编译没过）。第 1 轮还没跑过局（prev=None）→ 只有 human 那段。
    """
    if not prev:
        return human
    s = ("\n【上一局证据（第 %d 轮已跑完）】尺子判分: %s（退出码 %s）\n"
         "· 证据目录: %s（play.log 脚本每拍一行 / report.md 摘要+关键帧 / metrics.jsonl 外部帧差异 / session.json）\n"
         "· 上一次归档: %s\n"
         "· 尺子输出（末尾）:\n%s\n"
         % (prev["r"], "PASS" if prev["pass"] else "FAIL", prev["rc"], CUR,
            prev["archive"] or "(无)", (prev["out"] or "")[-4000:]))
    if prev.get("build_out"):
        s += ("⚠ 上一轮**编译没过**（脚本有语法/类型错，先修它）—— tsc 输出：\n%s\n"
              % prev["build_out"][-4000:])
    return s + human


def design_prompt(ctx, round_no, evidence):
    """**每轮只给增量**：这一轮要干什么 + 上一局的证据。

    环境简报与玩法任务书在系统提示的**常驻说明**里（见 write_agent_system）—— 它们整场不变，
    所以不再每轮重复贴；agent_cli 也不会每轮清上下文，历史本来就接着（见 MEMORY 的 agent_cli 上下文语义）。
    """
    return (
        "【任务】第 %d 轮：把 %s 写成 / 改成**能通关的一版**。\n"
        "%s"
        "⚠ 这一轮**只有这一件活**：先读【上一局证据】（第 1 轮没有），然后动手改脚本，改完**立刻停手**。\n"
        "任务书那套『① 探索 ② 设计脚本 ③ 运行 ④ 观测 ⑤ 调整』里，①③④ 都由**驱动**代劳：\n"
        "  你交棒 —— 驱动编译 → 在容器里跑一局 → 跑尺子 verify.py → 把结论与证据**带回下一轮的这个任务里**。\n"
        "⚠ 所以**每轮只有你一次机会**：这一轮交出去的脚本，就是这一轮被评判的那一版（把它写好）。\n"
        "⛔ **不要**自己编译/跑局，**不要**逐条 grep 源码、逐个目录 ls 核对环境、反复自查 ——\n"
        "  时间预算不够（曾经写完脚本后还在空转核对，把整轮拖过了一小时、回包都丢了）。\n"
        "  拿不准的地方写进脚本注释、留给下一轮的证据去判即可。写完回一句话总结；\n"
        "  **别写 `[script-ok]`** —— 收工只看尺子判分（通关就停，没通关就下一轮带着证据继续）。\n"
        "（玩法介绍 / 红线 / 判分口径 / 环境 在**系统提示的常驻说明**里，整场不变。）\n"
        % (round_no, ctx["play_ts"], evidence)
    )


# ==================== 一局（交给**题目自己的尺子**跑 + 判） ====================
def container_port(box):
    """容器 HTTP 端口（从 base url 里取；给玩法尺子用）。"""
    return int(str(box.c.base).rsplit(":", 1)[-1])


def one_round(box, ctx, round_no):
    """跑一局 + 判分 —— 交给题目的 `verify.py`：

    它自己清掉容器里的程序、装 game + play、从外面抓帧、读状态（score/over/win），
    给出 verdict（cleared / game_over / running / …）。驱动只负责把它跑起来、拿退出码、归档。
    """
    verify_path = ctx["verify"]
    if not os.path.exists(verify_path):
        print("FAIL: 找不到题目的尺子: %s" % verify_path)
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

    # 状态文件是**尺子**按题目 config.json 算的（任务根锚定），把它也抄一份到工作区，
    # 这样工作区里的证据是完整的（人不用两头翻）。
    st = ctx.get("status_path") or ""
    if st and os.path.exists(st):
        try:
            shutil.copy2(st, os.path.join(CUR, os.path.basename(st)))
        except Exception:
            pass

    # ---- 归档 ----
    archive = ""
    if ctx["archive"]:
        archive = os.path.join(RUNS, "round_%03d" % (round_index() + 1))
        os.makedirs(archive, exist_ok=True)
        for name in ("session.json", "metrics.jsonl", "result.json", "report.md", "play.log",
                     "play_stdout.txt", "container_stdout.txt", "agent_cli_stdout.txt",
                     "human.json", "Agent.log"):   # 人插过的手/agent 这一轮的回合诊断也是证据
            src = os.path.join(CUR, name)
            if os.path.exists(src):
                shutil.copy2(src, os.path.join(archive, name))
        if st and os.path.exists(st):
            shutil.copy2(st, os.path.join(archive, os.path.basename(st)))
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
        print("== 归档 -> %s" % rel_mix(archive, ROOT))
    return {"rc": rc, "out": out, "pass": passed, "archive": archive}


# ==================== 主流程 ====================
def normalize_settings(cfg):
    """配置文件（新旧两种形状都认：字段写在顶层、或写在 `run` 段里）→ 一份设置 dict（顶层优先）。"""
    S = {}
    if isinstance(cfg.get("run"), dict):
        S.update(cfg["run"])
    S.update({k: v for k, v in cfg.items() if k != "run"})
    return S


def plan_paths(test_dir, work_dir):
    """定下两个目录，并把派生路径写进模块级变量（helper 函数都在用）。"""
    global TEST_DIR, WORK_DIR, BUILD, RUNS, CUR, LIVE, LIVE_MIRROR, HUMAN, HUMAN_WORK
    TEST_DIR = os.path.abspath(test_dir)
    WORK_DIR = os.path.abspath(work_dir)
    BUILD = os.path.join(WORK_DIR, "build")
    RUNS = os.path.join(WORK_DIR, "runs")
    CUR = os.path.join(RUNS, "current")
    LIVE = os.path.join(CUR, "live.json")
    HUMAN_WORK = os.path.join(CUR, "human.json")
    # watch.py 住在玩法目录里，按"往上找到任务目录"找现场 ⇒ 任务目录那份也写/读
    LIVE_MIRROR = os.path.join(HERE, "runs", "current", "live.json")
    HUMAN = os.path.join(HERE, "runs", "current", "human.json")


def resolve_ctx(play, S):
    """题目侧 + 依赖 + 这一轮的 ctx —— **run.py 与 run_veirfy.py 共用**（纯计算：不打印、不落盘）。

    前置：`plan_paths()` 已经定下 TEST_DIR / WORK_DIR / BUILD（本函数要用）。
    返回 `(ctx, meta)`；meta 里是编译与容器要用的东西（tsc / exe / 本体 ts / 状态文件路径…）。
    """
    app_ts = E.play_file(TEST_DIR, play.get("app") or "", HERE)
    verify_path = E.play_file(TEST_DIR, play.get("verify") or "", HERE)
    prompt_file = os.path.join(TEST_DIR, "prompt.md")
    if not os.path.exists(prompt_file):
        prompt_file = E.play_file(TEST_DIR, S.get("prompt") or "", HERE)
    # 尺子的状态文件：它自己按题目 config.json 的 statusPath 算（**任务根锚定**，见 game/<玩法>/verify.py），
    # 驱动必须把 game.json 的 statusPath 写成同一个路径，否则"游戏写一边、尺子读另一边"。
    ruler_task = os.path.dirname(os.path.dirname(TEST_DIR))
    raw_status = str(play.get("statusPath") or "")
    status_path = raw_status if os.path.isabs(raw_status) else (
        os.path.join(ruler_task, raw_status) if raw_status else "")

    # 本体 JS：jsRoot=repo（本体住在别处）→ 用它自己工程里的产物；否则编到工作区 build/game/
    if str(play.get("jsRoot") or "task").lower() == "repo":
        game_js = E.play_js_path(play, HERE, BUILD)
        app_ts_for_build = ""
    else:
        game_js = os.path.join(BUILD, "game", E.play_js_name(play, app_ts))
        app_ts_for_build = app_ts

    cfgname = str(S.get("cfg") or "Debug")
    tsc = str((S.get("tsc") or {}).get("path") or "") or E.tsc_exe(cfgname)
    exe = E.container_exe(cfgname)
    dll_dir = S.get("dllDir") or E.emulator_dll_dir(cfgname)
    rom = S.get("rom") or DEFAULT_ROM
    ctx = {
        "play": play["play"], "app": play.get("app") or "", "program": play.get("program") or "",
        "window": play.get("window") or "", "need_rom": bool(play.get("rom")),
        "desktop": S.get("desktop") or play.get("desktop") or "320x240",
        "native": [int((play.get("native") or [0, 0])[0]), int((play.get("native") or [0, 0])[1])],
        # 节奏：配置显式写了才算覆盖，否则按题目自己 config.json 的
        "steps": int(S.get("steps") or play.get("steps") or 120),
        "step_ms": int(S.get("stepMs") or play.get("stepMs") or 400),
        "warmup": float(S.get("warmupSec") if S.get("warmupSec") is not None
                        else (play.get("warmupSec") or 3.0)),
        "seconds": float(S.get("seconds") or 0.0),
        "archive": bool(S.get("archive", True)),
        "verify": verify_path,
        "target": int(S.get("target") or play.get("targetScore") or 0),
        "play_ts": os.path.join(WORK_DIR, "play", "play.ts"),      # **产出在工作区**
        "play_js": os.path.join(BUILD, "play", "play.js"),
        "game_js": game_js,
        "status_path": status_path,
        "prompt_file": prompt_file if os.path.exists(prompt_file) else "",
        "rom": rom,
        "container_url": "",
    }
    meta = {"app_ts": app_ts, "app_ts_for_build": app_ts_for_build, "verify": verify_path,
            "prompt_file": prompt_file, "status_path": status_path, "game_js": game_js,
            "cfgname": cfgname, "tsc": tsc, "exe": exe, "dll_dir": dll_dir, "rom": rom}
    return ctx, meta


def main():
    ap = argparse.ArgumentParser(description="game_play 驱动：一份 run.json 说了算（见文件头注释）")
    ap.add_argument("run", nargs="?", default=os.path.join(HERE, "run.json"),
                    help="这次的运行配置（默认：本目录的 run.json）")
    ap.add_argument("--dry-run", action="store_true", help="只准备 + 编译 + 打印计划，不碰容器/agent")
    ap.add_argument("--no-agent", action="store_true", help="不起 agent，只跑局（调链路/复验脚本用）")
    args = ap.parse_args()

    # ---- 唯一的输入：这份运行配置（题目 + 工作区 + 与题目无关的调参）----
    cfg_path = resolve_from(args.run, os.getcwd())
    cfg = E.read_json(cfg_path, None)
    if not isinstance(cfg, dict):
        print("FAIL: 读不到运行配置、或它不是 JSON 对象: %s" % cfg_path)
        print("      例: python run.py auto_code\\game_play\\run.json")
        return 1
    S = normalize_settings(cfg)
    print("== 运行配置: %s" % rel_mix(cfg_path, ROOT))

    # ---- 两个目录（相对路径按**仓库根**解析 —— 同一份配置在哪儿调用都指向同一处）----
    test_dir = resolve_from(S.get("testDir") or "", ROOT)
    work_dir = resolve_from(S.get("workDir") or "", ROOT)
    if not test_dir or not work_dir:
        print("FAIL: 运行配置里要有 testDir 与 workDir: %s" % cfg_path)
        return 1
    if not os.path.isdir(test_dir):
        print("FAIL: 题目目录不存在: %s" % test_dir)
        return 1
    plan_paths(test_dir, work_dir)
    start_driver_log(sys.argv)     # 从这儿起 stdout 双写 <work>/driver.log（阶段/停止原因留档）
    try:
        play = E.play_config_dir(TEST_DIR, HERE)
    except Exception as ex:
        print("FAIL: %s" % ex)
        return 1

    print("== 题目: %s（玩法 %s）" % (rel_mix(TEST_DIR, ROOT), play["play"]))
    print("== 工作区: %s%s" % (rel_mix(WORK_DIR, ROOT), "（--dry-run）" if args.dry_run else ""))

    # ---- 题目侧 + 依赖 + ctx（与 run_veirfy.py 共用，见 resolve_ctx）----
    ctx, meta = resolve_ctx(play, S)
    app_ts = meta["app_ts"]
    app_ts_for_build = meta["app_ts_for_build"]
    status_path = meta["status_path"]
    cfgname, tsc, exe, dll_dir, rom = (meta["cfgname"], meta["tsc"], meta["exe"],
                                       meta["dll_dir"], meta["rom"])
    rounds_cfg = S.get("rounds") if isinstance(S.get("rounds"), dict) else {}
    max_rounds = int(rounds_cfg.get("max") or S.get("roundsMax") or 10)
    round_timeout = float(rounds_cfg.get("timeoutSec") or 3600)
    stall_rounds = int(rounds_cfg.get("stallRounds") or 3)
    retry_sec = float(rounds_cfg.get("retrySec") or 5)
    drain_sec = float(S.get("agentDrainSec") or 1800)
    max_minutes = float(S.get("maxMinutes") or 0)
    deadline = time.time() + max_minutes * 60 if max_minutes > 0 else None
    stop_when = str(S.get("stopWhen") or "verify-pass")     # rounds | verify-pass
    want_build = bool(S.get("build", True))
    agent_cfg = S.get("agent") if isinstance(S.get("agent"), dict) else {}
    if args.no_agent:
        agent_cfg = dict(agent_cfg, enabled=False)

    # ---- 依赖 ----
    need = [(exe, "容器 virtual_device.exe"), (tsc, "tsc")]
    if ctx["need_rom"]:
        need += [(os.path.join(dll_dir, "mgba_host.dll"), "mgba_host.dll"), (rom, "ROM")]
    for p, why in need:
        if not os.path.exists(p):
            print("FAIL: 找不到 %s: %s" % (why, p))
            return 1
    print("== 玩法: %s  进程名=%s 窗口=%s 桌面=%s 原生=%dx%d%s"
          % (ctx["play"], ctx["program"], ctx["window"], ctx["desktop"],
             ctx["native"][0], ctx["native"][1], "（要 ROM）" if ctx["need_rom"] else ""))
    print("== 尺子: %s" % rel_mix(ctx["verify"], ROOT))
    print("== 任务书: %s（%d 字）"
          % (rel_mix(ctx["prompt_file"], ROOT) if ctx["prompt_file"] else "(没找到 —— agent 拿不到)",
             len(read_text(ctx["prompt_file"]))))

    # ---- 工作区：脚手架 + 计划（每次跑都（重）生成）----
    game_tsconfig, notes = prepare_work_dir(play, app_ts_for_build, args.dry_run)
    for n in notes:
        print("   · " + n)
    print("== 产出: %s（编译产物 %s）" % (rel_mix(ctx["play_ts"], ROOT), rel_mix(ctx["play_js"], ROOT)))
    print("== 本体: %s" % rel_mix(ctx["game_js"], ROOT))

    bin_path = agent_bin_path(S)
    cli_cfg = write_agent_config(S, bin_path, TEST_DIR)
    plan = {
        "test_dir": TEST_DIR, "work_dir": WORK_DIR, "play": ctx["play"],
        "program": ctx["program"], "window": ctx["window"], "desktop": ctx["desktop"],
        "native": ctx["native"], "verify": ctx["verify"], "prompt_file": ctx["prompt_file"],
        "status_path": ctx["status_path"], "play_ts": ctx["play_ts"], "play_js": ctx["play_js"],
        "game_js": ctx["game_js"], "game_ts": app_ts if app_ts_for_build else "(题目自己的工程)",
        "play_ts_from": "空骨架（首次生成；之后保留工作区里的现有内容）",
        "build": {"play": os.path.join(WORK_DIR, "tsconfig.json"), "game": game_tsconfig or "(无)",
                  "prebuild": E.play_prebuild(play), "tsc": tsc, "cfg": cfgname},
        "agent": {"enabled": bool(agent_cfg.get("enabled", True)), "bin": bin_path,
                  "config": cli_cfg, "llm": (E.read_json(cli_cfg, {}) or {}).get("llm")},
        "rounds": {"max": max_rounds, "timeoutSec": round_timeout, "stallRounds": stall_rounds,
                   "retrySec": retry_sec, "agentDrainSec": drain_sec},
        "stopWhen": stop_when, "archive": ctx["archive"], "settings_from": S.get("_from") or cfg_path,
    }
    write_json(os.path.join(WORK_DIR, "plan.json"), plan)

    # ---- 编译（一次：题目本体 + 产出）----
    def compile_all():
        outs, rc = "", 0
        if not want_build:
            return 0, "(build=false，跳过编译)"
        for proj in E.play_prebuild(play):
            r2, o2 = run([tsc, "-p", proj], cwd=os.path.dirname(proj), check=False)
            outs += o2
            if r2 != 0:
                rc = r2
        if game_tsconfig:
            r2, o2 = run([tsc, "-p", game_tsconfig], cwd=WORK_DIR, check=False)
            outs += o2
            if r2 != 0:
                rc = r2
        r2, o2 = run([tsc, "-p", os.path.join(WORK_DIR, "tsconfig.json")], cwd=WORK_DIR, check=False)
        outs += o2
        if r2 != 0:
            rc = r2
        return rc, outs

    if args.dry_run:
        rc, out = compile_all()
        ok = (rc == 0) and (not want_build or (os.path.exists(ctx["play_js"]) and os.path.exists(ctx["game_js"])))
        print("\n== dry-run: 编译 %s（产物 %s / %s）"
              % ("OK" if rc == 0 else "失败(rc=%d)" % rc,
                 "有" if os.path.exists(ctx["play_js"]) else "缺",
                 "有" if os.path.exists(ctx["game_js"]) else "缺"))
        print("== 计划: %s" % rel_mix(os.path.join(WORK_DIR, "plan.json"), ROOT))
        print("== 工作区结构:")
        for root, dirs, files in os.walk(WORK_DIR):
            dirs[:] = [d for d in dirs if d not in ("node_modules",)]
            lvl = root[len(WORK_DIR):].count(os.sep)
            if lvl > 2:
                continue
            print("   %s%s/" % ("  " * lvl, os.path.basename(root) or "."))
            for f in sorted(files):
                print("   %s- %s" % ("  " * (lvl + 1), f))
        print("== dry-run 收工（没起容器、没起 agent）；下一步去掉 --dry-run 就是真跑")
        return 0 if ok else 1

    # ---- 现场文件：每次跑局开新的（否则 watch.py 会照着上一轮的地址去连）----
    os.makedirs(CUR, exist_ok=True)
    os.makedirs(os.path.dirname(LIVE_MIRROR), exist_ok=True)
    if status_path:       # 尺子把状态文件放在任务根（它自己按题目 config 算）—— 目录得在，否则游戏写不进去
        os.makedirs(os.path.dirname(status_path), exist_ok=True)
    for p in (LIVE, LIVE_MIRROR, HUMAN, HUMAN_WORK):
        try:
            os.remove(p)
        except FileNotFoundError:
            pass
        except Exception:
            fresh_file(p)   # 本机 python 的 os.remove 可能变成"移入回收站"而失败；清空一样能用

    taskbook = read_text(ctx["prompt_file"])
    last_verify = None
    agent_factory = None
    with ExitStack() as stack:
        # ---- ① 拉取 virtual_env ----
        box = stack.enter_context(
            E.HeadlessContainer(cfg=cfgname, desktop=ctx["desktop"], exe=exe,
                                log_path=os.path.join(CUR, "container_stdout.txt")))
        ctx["container_url"] = box.c.base
        publish_live(ctx, "container-ready", 0, box)

        # ---- ② 拉取 agent_cli（工作区 = WORK_DIR；题目目录只读挂进 trustedDirs）----
        agent = None
        if agent_cfg.get("enabled", True):
            if not bin_path or not os.path.exists(bin_path):
                msg = "找不到 agent_cli: %s（配 agent.bin）" % (bin_path or "(空)")
                if agent_cfg.get("required", True):
                    print("FAIL: " + msg)
                    return 1
                print("!! " + msg)
            else:
                def make_agent():
                    return E.AgentCliProc(bin_path, config_path=cli_cfg,
                                          http_url=agent_cfg.get("httpUrl", ""),
                                          token=str(agent_cfg.get("token", "")),
                                          ready_sec=int(agent_cfg.get("readySec", 120)))

                agent_factory = make_agent
                agent = agent_factory()
                stack.callback(agent.stop)
                # 常驻说明(环境简报 + 玩法任务书)必须在 agent_cli 启动**之前**写好 —— 它启动时读一次,
                # 之后拼进 system prompt; 每轮 /task 只叠加"任务 + 上一局证据"。
                sys_md = write_agent_system(ctx, taskbook)
                print("== 常驻说明(system prompt): %s（%d 字）"
                      % (rel_mix(sys_md, ROOT), len(read_text(sys_md))))
                if not agent.start(log_path=os.path.join(CUR, "agent_cli_stdout.txt")):
                    if agent_cfg.get("required", True):
                        print("FAIL: agent_cli 没起来（agent.required=true）")
                        return 1
                    print("!! agent_cli 没起来 —— 只跑局，不驱动 agent")
                    agent = None
                else:
                    info = agent.info() or {}
                    print("== agent_cli READY: %s（工作区=%s；system prompt %s 字节，常驻说明 %s）"
                          % (agent.url, rel_mix(WORK_DIR, ROOT),
                             info.get("systemPromptBytes", "?"), info.get("systemExtraFile") or "(无)"))
                    publish_live(ctx, "agent-ready", 0, box, agent)
        else:
            print("!! agent.enabled=false：不驱动 agent（只跑局）")

        # ---- ③ 循环（**每轮只有一次 LLM 回合**：设计/改脚本，上一局的证据随任务一起给）----
        stall, r, last_sig, prev = 0, 0, None, None
        stop_reason = ""
        while r < max_rounds and (deadline is None or time.time() < deadline):
            r += 1
            print("\n================ 第 %d 轮 ================" % r)
            publish_live(ctx, "design", r, box, agent)

            # a) 设计 / 改进脚本（带上一局的证据 —— 见 evidence_block）
            if agent:
                # 人插过的手（watch.py 直接给这个 agent 下过任务）也带进这一轮 ——
                # 否则它按"上一轮的认知"动手，跟被别人改过的文件现状打架。
                hn = human_note()
                if hn:
                    print("== 检测到人类干预（human.json），已带进这一轮上下文")
                evidence = evidence_block(ctx, prev, hn)
                try:
                    resp = agent.submit_task(design_prompt(ctx, r, evidence),
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
                    # ⚠ /task 是"连接保持"式的：**我们这边超时断开不会杀掉 agent 手里的任务**
                    #   （JZAgentCli 的 onClientDestroyed 只把回包通道置空，任务照跑）。它这时
                    #   可能还在改产出脚本 ⇒ 编译/跑局之前先等它收尾，否则就是拿一个**还在变**的
                    #   文件做实验（下一轮 POST 也会撞 409）。
                    if agent and agent.alive() and agent.busy():
                        print("== agent 手里还有任务在跑（本轮回包丢了），等它收尾再编译…")
                        if not agent.wait_idle(timeout=drain_sec):
                            print("!! 等 agent 收尾超时（%ds），仍然继续" % drain_sec)
                    time.sleep(retry_sec)

            # b) 编译（agent 改的是 .ts；编译不过就把错误交给**下一轮的设计任务**）
            rc, build_out = compile_all()
            build_ok = (rc == 0) or (not want_build)
            if build_ok:
                build_ok = os.path.exists(ctx["play_js"]) and os.path.exists(ctx["game_js"])
            if not build_ok:
                print("!! 编译失败（或产物缺失）—— 跳过跑局，把错误交给 agent 修")

            # c) 跑一局 + d) 判分 + 归档
            res = one_round(box, ctx, r) if build_ok else \
                {"rc": 1, "out": "（编译没过，本轮没跑局）", "pass": False, "archive": ""}
            last_verify = (r, res["pass"], res["archive"])

            # 这一局的结论/证据留给下一轮的设计任务（每轮只有一次 LLM 回合 ⇒ 必须自己带着走）
            prev = {"r": r, "rc": res["rc"], "out": res["out"], "pass": res["pass"],
                    "archive": res["archive"], "build_out": "" if build_ok else build_out}

            # e) 出口
            if stop_when == "verify-pass" and res["pass"]:
                print("== 尺子 PASS，按 stopWhen=verify-pass 收工")
                stop_reason = "verify-pass（第 %d 轮通关）" % r
                break
            sig = (file_sig(ctx["play_ts"]), res["rc"])
            if sig == last_sig:
                stall += 1
                print("== 没进展（脚本没改 且 判分没变）: %d/%d" % (stall, stall_rounds))
                if stall >= stall_rounds:
                    print("== 连续 %d 轮没进展，收工" % stall)
                    stop_reason = "stall（连续 %d 轮没进展）" % stall
                    break
            else:
                stall = 0
                last_sig = sig

        if not stop_reason:      # 循环条件自然到点（没走到上面任何一个 break）
            stop_reason = "max-minutes（到时间上限）" if (deadline is not None and time.time() >= deadline) \
                else "rounds-max（到轮次上限 %d）" % max_rounds
        publish_live(ctx, "stopped", r, box, agent, extra={"stop_reason": stop_reason})
        print("\n== 收工（%s）: 跑了 %d 轮；最后一次判分 %s；最终脚本 %s"
              % (stop_reason, r, "PASS" if (last_verify and last_verify[1]) else "FAIL", ctx["play_ts"]))
        print("== 工作区: %s（证据 runs/current，归档 runs/round_NNN，计划 plan.json）" % WORK_DIR)
    return 0


if __name__ == "__main__":
    sys.exit(main())
