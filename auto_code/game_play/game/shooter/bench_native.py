#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""原生桌面基准：**真桌面** + C++ 版 shooter 应用 + `code_runtime` 跑脚本。

与容器那条路（verify.py = 尺子；build/live_shooter/bench_play.py = 多局基准）的对应关系：
  · 容器版 = 虚拟桌面 + JS 版游戏(容器里的程序) + 尺子读游戏 stdout；
  · 本脚本 = **原生桌面**(computeruse 的默认后端) + C++ 版 `test_shooter.exe` + **没有尺子** ——
    两个进程就是全部：一个应用、一个 `code_runtime --entry play.js`。
    分数从**游戏自己**打的 `[shooter] score=..` / `CLEAR` / `GAME OVER` 行里读（那是给尺子的通道；
    脚本按红线不许读它，它只看画面）。C++ 版与 TS 版的行格式是同一套（见 game_widget.cpp 的 reportState）。

⚠ 这不是"考试"：它是**真机复验**那件事的脚手架 —— computeruse 的设计承诺是"虚拟桌面里跑通的脚本
  能在真机上复验"（见 JZComputerUse.h 的类注释），这个脚本就是执行那句承诺、并给出可比数字的地方。
  ⚠ 真桌面与虚拟桌面有**硬差别**，脚本侧必须自己适配（这一轮实测踩到的三处已写进 play.ts）：
    ① 别处也有同色像素（任务栏/IDE）⇒ 搜索区必须收进目标窗口客户区；
    ② 目标在别的进程、没有可访问布局 ⇒ "窗口在不在"要查窗口表, 不能只问 layoutTree；
    ③ 窗口不一定在屏幕中间 ⇒ 激活要点它客户区中心, 不能点屏幕正中心。

每局流程：
  1. 收掉上一局留下的 `test_shooter.exe`（按进程名，它只可能是我们起的）；
  2. 起应用，等它把窗口画出来；
  3. 给脚本写 play.json（out/window/steps/stepMs —— 脚本按 jzApp.applicationDirPath() 找它）；
  4. 起 `code_runtime --entry <脚本产物>`，两边 stdout 都落盘；
  5. 等脚本跑完（或超时）后收掉两边，解析日志给出 最高分/通关/挨打次数。

⚠ 这一轮会**占住真桌面的键盘与鼠标**（脚本要按住方向键 + 空格，还要点一下游戏窗口）。
  跑的时候人别在同一块桌面上操作；脚本收工时会抬起按键（releaseAll），应用会被收掉。

产物: build/live_shooter/bench_native/<前缀><局号>/{app.log(游戏侧), play.log(脚本自写), rt_stdout.log, frames/}

用法:
  python auto_code/game_play/game/shooter/bench_native.py 1        # 跑 1 局
  python auto_code/game_play/game/shooter/bench_native.py 3 --steps 120 --step-ms 400
  python auto_code/game_play/game/shooter/bench_native.py 1 --steps 3 --step-ms 1000   # 几秒钟的链路自检
"""
import argparse
import os
import re
import subprocess
import sys
import time

ROOT = r'E:\JZFramework'
OUT_ROOT = os.path.join(ROOT, 'build', 'live_shooter', 'bench_native')
DEFAULT_APP = os.path.join(ROOT, 'build', 'test', 'widget', 'shooter', 'Release', 'test_shooter.exe')
DEFAULT_RUNTIME = os.path.join(ROOT, 'build', 'binary', 'code_agent', 'bin', 'Release', 'code_runtime.exe')
DEFAULT_PLAY_JS = os.path.join(ROOT, 'build', 'auto_code', 'game_play', 'shooter',
                               'build', 'verify', 'play', 'play.js')
APP_NAME = 'test_shooter.exe'

# 窗口标题与玩法配置(config.json)一致 —— C++ 样例的标题就是这个。
WINDOW = 'JZWidget GDI - 飞行射击'

sys.path.insert(0, os.path.join(ROOT, 'auto_code', 'game_play'))
import env as E       # noqa: E402

SCORE_RE = re.compile(r'\[shooter\] score=(\d+) lives=(\d+)')
ALIVE_RE = re.compile(r'\[shooter\] alive score=(\d+) lives=(\d+)')
CLEAR_RE = re.compile(r'\[shooter\] CLEAR score=(\d+)')
OVER_RE = re.compile(r'\[shooter\] GAME OVER score=(\d+)')
CALIB_RE = re.compile(r'游戏区 = \([-\d,]+ [\dx.]+\) 缩放 SC=([\d.]+)')
DONE_RE = re.compile(r'\[shooter-vision\] 收工: (.*)')


def read_text(path):
    try:
        with open(path, 'rb') as f:
            return f.read().decode('utf-8', 'replace')
    except Exception:
        return ''


def kill_app():
    subprocess.run(['taskkill', '/F', '/IM', APP_NAME], capture_output=True)


def run_round(i, app_exe, runtime, play_js, steps, step_ms, warmup, prefix='n'):
    out = os.path.join(OUT_ROOT, '%s%02d' % (prefix, i))
    os.makedirs(out, exist_ok=True)
    app_log = os.path.join(out, 'app.log')
    # ⚠ 脚本**自己也**写一个 play.log(它按 fs.appendFileSync 落在 out 目录) ⇒ 这边别再叫这个名字,
    #   否则两个写者交叉写同一个文件(实测读出来是乱的)。
    rt_log = os.path.join(out, 'rt_stdout.log')
    script_log = os.path.join(out, 'play.log')
    for p in (app_log, rt_log, script_log):
        if os.path.exists(p):
            os.remove(p)

    kill_app()
    time.sleep(0.3)

    # 1) 应用
    t0 = time.time()
    with open(app_log, 'wb') as fa:
        app = subprocess.Popen([app_exe], stdout=fa, stderr=subprocess.STDOUT, cwd=os.path.dirname(app_exe))
        time.sleep(warmup)                      # 等它把窗口画出来(脚本那边还会等 15s, 这里只是省时间)
        if app.poll() is not None:
            return dict(score=-1, detail='应用起来就退了(exit=%s)' % app.returncode)

        # 2) 脚本参数(写在脚本产物旁边)
        E.write_play_config(os.path.dirname(play_js), out, window=WINDOW, warmup_sec=1.0,
                            steps=steps, step_ms=step_ms, max_seconds=0.0, app='test_shooter')

        # 3) 脚本(code_runtime)。⚠ --entry = 脚本产物本身, 运行根 = 它所在目录
        with open(rt_log, 'wb') as fr:
            rt = subprocess.Popen([runtime, '--entry', play_js], stdout=fr, stderr=subprocess.STDOUT,
                                  cwd=os.path.dirname(play_js))
            budget = steps * step_ms / 1000.0 + 30.0 + warmup
            t_end = time.time() + budget
            while time.time() < t_end and rt.poll() is None:
                time.sleep(0.25)
            timed_out = rt.poll() is None
            if timed_out:
                rt.kill()
            rt.wait(timeout=10)

    app.terminate()
    try:
        app.wait(timeout=5)
    except Exception:
        subprocess.run(['taskkill', '/F', '/PID', str(app.pid)], capture_output=True)
    kill_app()

    # 4) 解析。星标那几行(校准/收工)从**脚本自己写的 play.log** 里读 —— 它是 UTF-8 落盘的,
    #    比抓 stdout 稳(console.log 那条路过控制台编码, 中文可能变成别的码页)。
    a = read_text(app_log)
    p = read_text(script_log) + read_text(rt_log)
    scores = [int(m.group(1)) for m in SCORE_RE.finditer(a)]
    clears = [int(m.group(1)) for m in CLEAR_RE.finditer(a)]
    overs = [int(m.group(1)) for m in OVER_RE.finditer(a)]
    hits = len(re.findall(r'\[hit\]', a))
    calib = CALIB_RE.search(p)
    done = DONE_RE.search(p)
    detail = ('通关 score=%d' % clears[0]) if clears else \
             (('GAME OVER score=%d' % overs[-1]) if overs else '(没读到判定)')
    if timed_out:
        detail += ' [超时]'
    return dict(score=(max(scores) if scores else -1), detail=detail, hits=hits,
                alive=len(ALIVE_RE.findall(a)), calib=(calib.group(1) if calib else '-'),
                done=(done.group(1).strip()[:110] if done else '(脚本没打收工行)'),
                elapsed=time.time() - t0)


def main():
    ap = argparse.ArgumentParser(description='原生桌面基准(见文件头)')
    ap.add_argument('n', nargs='?', type=int, default=1, help='跑多少局')
    ap.add_argument('--app', default=DEFAULT_APP, help='C++ 版应用(默认 Release test_shooter.exe)')
    ap.add_argument('--runtime', default=DEFAULT_RUNTIME, help='code_runtime(默认 Release)')
    ap.add_argument('--play-js', default=DEFAULT_PLAY_JS, help='脚本产物(默认复验区的 play.js)')
    ap.add_argument('--steps', type=int, default=120, help='脚本跑多少拍(与玩法配置一致)')
    ap.add_argument('--step-ms', type=int, default=400, help='每拍多少毫秒')
    ap.add_argument('--warmup', type=float, default=2.5, help='起应用后先等多久(秒)')
    ap.add_argument('--prefix', default='n', help='本批产物目录前缀(bench_native/<前缀><局号>)')
    args = ap.parse_args()

    for name, path in (('应用', args.app), ('runtime', args.runtime), ('脚本', args.play_js)):
        if not os.path.exists(path):
            print('!! %s 不存在: %s' % (name, path))
            return 2
    os.makedirs(OUT_ROOT, exist_ok=True)
    print('应用 = %s' % os.path.relpath(args.app, ROOT))
    print('脚本 = %s' % os.path.relpath(args.play_js, ROOT))
    print('窗口 = "%s"  ｜ %d 拍 x %dms = %.0fs/局' % (WINDOW, args.steps, args.step_ms,
                                                  args.steps * args.step_ms / 1000.0))

    rows = []
    for i in range(1, args.n + 1):
        r = run_round(i, args.app, args.runtime, args.play_js, args.steps, args.step_ms,
                      args.warmup, args.prefix)
        rows.append(r)
        print('  %s%02d 最高分=%-4s %-28s 挨打=%-2s 校准SC=%-6s (%.1fs)'
              % (args.prefix, i, r['score'], r['detail'], r.get('hits', '-'),
                 r.get('calib', '-'), r.get('elapsed', 0)))
        if r.get('done'):
            print('        脚本: %s' % r['done'])
    good = [r['score'] for r in rows if r['score'] >= 0]
    if good:
        print('\n== %d 局 ｜ 最高分 MAX=%d ｜ 各局: %s' % (len(good), max(good), good))
    return 0


if __name__ == '__main__':
    sys.exit(main())
