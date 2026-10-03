#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
game_play —— **只跑一局 + 尺子**（调脚本用；不起 agent、不进轮次循环）

用法（输入就两样）:
  python run_veirfy.py <run.json> <play.ts>     # 用这份运行配置，把这份脚本装进去跑一局
  python run_veirfy.py <run.json>              # 不给脚本 = 用工作区里的产出 <work>/play/play.ts

  python auto_code/game_play/run_veirfy.py auto_code/game_play/run.json my_play.ts

跟 `run.py` 吃**同一份 run.json**（同一套解析：相对路径按仓库根），所以"题目 / 工作区 / 调参"
完全一致 —— 区别只有三件事：不起 agent、只跑一轮、证据落在 `<work>/runs/verify/`（不覆盖驱动那份）。

它做什么:
  ① 复验区 `<work>/verify/`：拷入那份脚本（**不动**工作区里的产出）+ 类型模板 + 一份 tsconfig
  ② 编译：题目本体 → `<work>/build/game/`；复验脚本 → `<work>/build/verify/play/play.js`
  ③ 起容器 → 交给**题目自己的** `verify.py` 跑一局并判定（它自己装 game + play、外面抓帧、读状态）
  ④ 退出码 = 尺子的退出码（0 = 这道题算过）
"""

import argparse
import os
import shutil
import sys

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
import env as E       # noqa: E402
import run as R       # noqa: E402  （共用：路径解析 / ctx / 一局 / 工作区脚手架）


def write_verify_project(verify_dir, play_ts_src, notes):
    """复验区：`play/play.ts`（拷进来）+ `play/jzapi.d.ts` + 一份 tsconfig（产出 → build/verify）。"""
    pdir = os.path.join(verify_dir, "play")
    os.makedirs(pdir, exist_ok=True)
    shutil.copy2(play_ts_src, os.path.join(pdir, "play.ts"))
    tpl = os.path.join(HERE, "jzapi.d.ts")
    if os.path.exists(tpl):
        shutil.copy2(tpl, os.path.join(pdir, "jzapi.d.ts"))
    else:
        notes.append("!! 找不到类型模板 %s（脚本会缺 __jz 声明）" % tpl)
    task_ts = R.task_tsconfig()
    co = dict(task_ts.get("compilerOptions") or {})
    co.update({"outDir": "../build/verify", "rootDir": ".", "noEmitOnError": True, "sourceMap": False})
    R.write_json(os.path.join(verify_dir, "tsconfig.json"), {
        "compilerOptions": co,
        "include": ["play/**/*.ts", "play/**/*.d.ts"] + R._type_globs(task_ts, verify_dir),
    })
    return os.path.join(verify_dir, "tsconfig.json")


def main():
    ap = argparse.ArgumentParser(description="game_play 只跑一局 + 尺子（见文件头注释）")
    ap.add_argument("run", nargs="?", default=os.path.join(HERE, "run.json"),
                    help="这次的运行配置（默认：本目录的 run.json）")
    ap.add_argument("play_ts", nargs="?", default="",
                    help="要复验的脚本（默认：工作区里的产出 <work>/play/play.ts）")
    args = ap.parse_args()

    # ---- ① 运行配置（跟 run.py 同一份）----
    cfg_path = R.resolve_from(args.run, os.getcwd())
    cfg = E.read_json(cfg_path, None)
    if not isinstance(cfg, dict):
        print("FAIL: 读不到运行配置、或它不是 JSON 对象: %s" % cfg_path)
        return 1
    S = R.normalize_settings(cfg)
    test_dir = R.resolve_from(S.get("testDir") or "", ROOT)
    work_dir = R.resolve_from(S.get("workDir") or "", ROOT)
    if not test_dir or not work_dir or not os.path.isdir(test_dir):
        print("FAIL: run.json 里的 testDir/workDir 不对: %s" % cfg_path)
        return 1
    R.plan_paths(test_dir, work_dir)
    play = E.play_config_dir(R.TEST_DIR, HERE)
    ctx, meta = R.resolve_ctx(play, S)
    print("== 运行配置: %s" % R.rel_mix(cfg_path, ROOT))
    print("== 题目: %s（玩法 %s）" % (R.rel_mix(R.TEST_DIR, ROOT), play["play"]))
    print("== 工作区: %s" % R.rel_mix(R.WORK_DIR, ROOT))

    # ---- ② 复验区 + 工作区脚手架（题目本体的编译口径从这里来）----
    game_tsconfig, notes = R.prepare_work_dir(play, meta["app_ts_for_build"])
    verify_dir = os.path.join(R.WORK_DIR, "verify")
    play_ts_src = R.resolve_from(args.play_ts, os.getcwd()) if args.play_ts else ctx["play_ts"]
    if not os.path.exists(play_ts_src):
        print("FAIL: 找不到要复验的脚本: %s" % play_ts_src)
        return 1
    verify_tsconfig = write_verify_project(verify_dir, play_ts_src, notes)
    play_js = os.path.join(R.BUILD, "verify", "play", "play.js")
    print("== 复验脚本: %s（照抄进 %s）" % (R.rel_mix(play_ts_src, ROOT), R.rel_mix(verify_dir, ROOT)))

    # ---- ③ 编译：题目本体 + 复验脚本 ----
    rc = 0
    for proj in E.play_prebuild(play):
        r2, _ = R.run([meta["tsc"], "-p", proj], cwd=os.path.dirname(proj), check=False)
        rc = rc or r2
    for proj in (game_tsconfig, verify_tsconfig):
        if proj:
            r2, _ = R.run([meta["tsc"], "-p", proj], cwd=R.WORK_DIR, check=False)
            rc = rc or r2
    if rc != 0 or not os.path.exists(play_js) or not os.path.exists(ctx["game_js"]):
        print("FAIL: 编译没过（或产物缺失）—— 先修 TS 再来")
        return rc or 1

    # ---- ④ 起容器 → 题目自己的 verify.py 跑一局并判定 ----
    R.CUR = os.path.join(R.RUNS, "verify")        # 证据落这儿，不动驱动那份 runs/current
    os.makedirs(R.CUR, exist_ok=True)
    # ⚠ 脚本自己留帧的地方（cfg.out == R.CUR）：驱动跑时会建它，复验这条路以前没建 ——
    #   于是 shot() 全报 EACCES（判定不受影响，但"看画面"的证据全丢）。
    os.makedirs(os.path.join(R.CUR, "frames"), exist_ok=True)
    if ctx["status_path"]:
        os.makedirs(os.path.dirname(ctx["status_path"]), exist_ok=True)
    ctx["archive"] = False                        # 复验不归档
    ctx["play_js"] = play_js
    with E.HeadlessContainer(cfg=meta["cfgname"], desktop=ctx["desktop"], exe=meta["exe"],
                             log_path=os.path.join(R.CUR, "container_stdout.txt")) as box:
        ctx["container_url"] = box.c.base
        print("== 容器: %s（桌面 %s）" % (box.c.base, ctx["desktop"]))
        res = R.one_round(box, ctx, 1)
    print("== 复验结束: 尺子退出码 %s（证据 %s）"
          % (res["rc"], R.rel_mix(R.CUR, ROOT)))
    return res["rc"]


if __name__ == "__main__":
    sys.exit(main())
