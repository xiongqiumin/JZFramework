#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
game_play 任务 —— **单独跑一个玩法的尺子**（不起 agent、不跑编排循环）

用法:
  python run_veirfy.py                                 # 默认玩法（task.json 的 defaultPlay）
  python run_veirfy.py --play hello                    # 指定玩法
  python run_veirfy.py --script game/hello/play.ts     # 指定要装的脚本（缺省见下）
  python run_veirfy.py --no-build                      # 不编译（用已有产物）
  python run_veirfy.py --steps 60 --step-ms 200        # 覆盖观察节奏（缺省取玩法配置）
  python run_veirfy.py --keep                          # 尺子自起的容器测完不杀（接着手工看）

它做的三步:
  ① 编译: 任务工程(tsconfig.json) + 玩法 config.json 里 `prebuild` 列的**别的工程**
     （比如 shooter 的本体住在 binary/code_agent/runtime/test_jsshooter）
  ② 挑脚本: `game/<玩法>/play.ts` 有就用它（**玩法自带的参考脚本**），
     否则用产出物 `play/play.ts`
  ③ 调该玩法自己的 `verify.py` —— 它自己起容器 → 装本体 + 脚本 → 观察 → 判定，
     本文件只把它的结论与退出码原样透出来

⚠ 它**不做**: 不拉 agent_cli、不改脚本、不归档 runs/round_NNN —— 那些是 `run.py` 的活。
⚠ 判定口径按玩法自定（hello 要通关才算过；shooter 只要跑起来）—— 见各玩法 verify.py 的说明。
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(HERE, "build")
CUR = os.path.join(HERE, "runs", "current")
sys.path.insert(0, HERE)
import env as E  # noqa: E402


def run(cmd, cwd=None):
    """跑一条命令并把它的输出原样透出来；返回退出码。"""
    print("+ " + " ".join(cmd))
    p = subprocess.run(cmd, cwd=cwd, capture_output=True)
    out = (p.stdout or b"").decode("utf-8", "replace") + (p.stderr or b"").decode("utf-8", "replace")
    if out.strip():
        print(out.rstrip())
    return p.returncode


def pick_script(play_name, given):
    """要装的脚本：命令行 > `game/<玩法>/play.ts`（玩法自带的参考脚本）> 产出物 `play/play.ts`。"""
    if given:
        return given, "命令行指定"
    local = "game/%s/play.ts" % play_name
    if os.path.exists(os.path.join(HERE, local)):
        return local, "玩法目录自带"
    return "play/play.ts", "产出物"


def main():
    ap = argparse.ArgumentParser(description="单独跑一个玩法的尺子（见文件头）")
    ap.add_argument("--play", default="", help="玩法名（缺省取 task.json 的 defaultPlay）")
    ap.add_argument("--script", default="", help="要装的脚本（任务目录内相对路径）")
    ap.add_argument("--cfg", default="Debug", help="构建配置（找 tsc 用）")
    ap.add_argument("--no-build", action="store_true", help="跳过编译")
    ap.add_argument("--steps", type=int, default=0, help="观察拍数（0 = 玩法配置）")
    ap.add_argument("--step-ms", type=int, default=0, help="每拍间隔（0 = 玩法配置）")
    ap.add_argument("--warmup", type=float, default=-1.0, help="装脚本后等多久（-1 = 玩法配置）")
    ap.add_argument("--keep", action="store_true", help="尺子自起的容器测完不杀")
    args = ap.parse_args()

    try:
        play = E.play_config(args.play or "", base_dir=HERE)
    except Exception as ex:
        print("FAIL: %s" % ex)
        return 1
    name = play["play"]
    print("== 玩法: %s（本体 %s）" % (name, play.get("app") or "?"))

    # ---- ① 编译: 任务工程 + 玩法声明的"别的工程" ----
    if not args.no_build:
        tsc = E.tsc_exe(args.cfg)
        if not os.path.exists(tsc):
            print("FAIL: 找不到 tsc: %s" % tsc)
            return 1
        rc = run([tsc, "-p", os.path.join(HERE, "tsconfig.json")], cwd=HERE)
        for proj in E.play_prebuild(play):
            rc2 = run([tsc, "-p", proj], cwd=os.path.dirname(proj))
            if rc2 != 0:
                rc = rc2
        if rc != 0:
            print("FAIL: 编译没过（先修 TS 再跑）")
            return rc

    # ---- ② 挑脚本 ----
    script, why = pick_script(name, args.script)
    script_abs = script if os.path.isabs(script) else os.path.join(HERE, script)
    if not os.path.exists(script_abs):
        print("FAIL: 找不到脚本: %s" % script_abs)
        return 1
    rel = os.path.relpath(script_abs, HERE).replace("\\", "/")
    play_js = os.path.join(BUILD, os.path.splitext(rel)[0] + ".js")   # outDir=build, rootDir=. ⇒ 结构照搬
    if not os.path.exists(play_js):
        print("FAIL: 脚本产物不在: %s（先编译；或 tsconfig 的 include 没覆盖它）" % play_js)
        return 1
    print("== 脚本: %s（%s）→ %s" % (rel, why, os.path.relpath(play_js, HERE)))

    # ---- ③ 交给玩法自己的尺子（它自起容器、自己装、自己判）----
    verify = play.get("verify") or ""
    verify_abs = verify if os.path.isabs(verify) else os.path.join(HERE, verify)
    if not os.path.exists(verify_abs):
        print("FAIL: 这个玩法没有尺子: %s" % (verify_abs or "(没配)"))
        return 1

    cmd = [sys.executable, verify_abs, "--play", name, "--out", CUR,
           "--game-js", E.play_js_path(play, HERE, BUILD), "--play-js", play_js]
    if args.steps:
        cmd += ["--steps", str(args.steps)]
    if args.step_ms:
        cmd += ["--step-ms", str(args.step_ms)]
    if args.warmup >= 0:
        cmd += ["--warmup", str(args.warmup)]
    if args.keep:
        cmd += ["--keep"]
    print("== 跑尺子: %s" % os.path.relpath(verify_abs, HERE))
    rc = run(cmd, cwd=HERE)
    print("== 尺子退出码 %d（判定口径见该玩法的 verify.py）" % rc)
    return rc


if __name__ == "__main__":
    sys.exit(main())
