"""run_repair_live.py —— 一键驱动"实机 + 自动修改"自检（checks/repair_live.ts）

它做四件事：
  ① 编译 TS（tsc -p auto_code/computer_use/tsconfig.json）
  ② 准备模型端点：--fake(默认) 起本地假模型；--url 用真端点；--no-model 只验前 5 步
  ③ 跑 code_runtime --entry build/checks/repair_live.js
  ④ 退出码 = 自检结果（硬门全过才 0）

用法:
  python auto_code/computer_use/checks/run_repair_live.py               # 假模型（不依赖 GPU 机）
  python auto_code/computer_use/checks/run_repair_live.py --url http://<模型主机>:8080/v1/chat/completions --model <模型名>
  python auto_code/computer_use/checks/run_repair_live.py --no-model    # 只验 L1~L5
"""
import argparse
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
TASK = os.path.dirname(HERE)                       # auto_code/computer_use
ROOT = os.path.dirname(os.path.dirname(TASK))      # 仓库根
RUNS = os.path.join(TASK, "runs", "current")
RUNTIME = os.path.join(ROOT, "build", "binary", "bin", "Debug", "code_runtime.exe")
TSC = os.path.join(ROOT, "build", "binary", "bin", "Debug", "ts_compiler", "tsc", "tsc.exe")
ENTRY = os.path.join(TASK, "build", "checks", "repair_live.js")
FAKE_PORT = 8731


def run(cmd, **kw):
    print("+ " + " ".join(cmd), flush=True)
    return subprocess.run(cmd, **kw)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default="")
    ap.add_argument("--model", default="my_model")
    ap.add_argument("--fake", action="store_true")
    ap.add_argument("--no-model", action="store_true")
    ap.add_argument("--no-tsc", action="store_true")
    a = ap.parse_args()

    os.makedirs(RUNS, exist_ok=True)

    if not a.no_tsc:
        r = run([TSC, "-p", os.path.join(TASK, "tsconfig.json")])
        if r.returncode != 0:
            print("tsc 失败", file=sys.stderr)
            return 1

    fake = None
    if a.no_model:
        url = ""
    elif a.fake or not a.url:
        fake = subprocess.Popen([sys.executable, os.path.join(HERE, "fake_repair_model.py"), str(FAKE_PORT)])
        url = f"http://127.0.0.1:{FAKE_PORT}/v1/chat/completions"
        # 等它起来
        for _ in range(30):
            try:
                urllib.request.urlopen(f"http://127.0.0.1:{FAKE_PORT}/v1/chat/completions", b"{}", timeout=1)
                break
            except urllib.error.HTTPError:
                break
            except Exception:
                time.sleep(0.2)
    else:
        url = a.url

    cfg_path = os.path.join(RUNS, "repair.json")
    with open(cfg_path, "w", encoding="utf-8") as f:
        json.dump({"url": url, "model": a.model}, f, ensure_ascii=False, indent=1)
    print("== 模型端点: " + (url or "(关闭)"), flush=True)

    try:
        rc = run([RUNTIME, "--entry", ENTRY]).returncode
    finally:
        if fake:
            fake.terminate()
    print(f"== exit={rc}" + ("  PASS" if rc == 0 else "  FAIL"), flush=True)
    return rc


if __name__ == "__main__":
    sys.exit(main())
