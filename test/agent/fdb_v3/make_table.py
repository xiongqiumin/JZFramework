#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
把结果目录里已有的 result_<provider>.json 批量评测，汇总成 FDB-v3 论文/README 那张表：

    模型 | Tool Sel↑ | Arg Acc↑ | Resp Qual↑ | Pass@1↑ | Take-turn↑ | Latency↓

各列来源（全部复用上游脚本，不自己造指标）：
    Tool Sel / Arg Acc / Resp Qual / Take-turn  <- evaluate_tool_calls.py 的 report
    Pass@1                                      <- evaluate_pass_rate.py 的 overall_pass_rate
    Latency                                     <- result 里的 perceived_total_latency（语音产物才有）

⚠ 三个坑：
  1. Resp Qual 与"语义版" Arg Acc 都依赖 gpt-4o judge（`--use-llm`）。不开就是 exact-match：
     Resp Qual = None，Arg Acc 偏低（`July 15` vs `2026-07-15` 这种会被判错）。本脚本默认不开启，
     需要时加 `--use-llm`（前提：pip install openai + 有能应答的 gpt-4o 端点）。
  2. Latency 是**音频时序**（用户说完 → agent 出声），文本模式没有 → 显示 "-"，与语音模型不可比。
  3. 上游脚本 print emoji，GBK 控制台会 UnicodeEncodeError ⇒ 本脚本给子进程强制 UTF-8。

用法：
    python make_table.py --results-dir C:\\Users\\7052\\Desktop\\tmp\\fdb_v3_data_released
    python make_table.py --providers cpp_text,cascTxt --out table.md
"""

import argparse
import glob
import json
import os
import subprocess
import sys

DEFAULT_UPSTREAM = r"C:\Users\7052\Desktop\tmp\fdb_v3_upstream"


def discover_providers(results_dir: str):
    """扫 result_<provider>.json，统计每个 provider 的样本数。"""
    counts = {}
    for f in glob.glob(os.path.join(results_dir, "*", "result_*.json")):
        name = os.path.basename(f)
        provider = name[len("result_"):-len(".json")]
        counts[provider] = counts.get(provider, 0) + 1
    return counts


def run_upstream(script: str, upstream: str, benchmark: str, results_dir: str,
                 provider: str, out_path: str, use_llm: bool) -> bool:
    cmd = [sys.executable, os.path.join(upstream, script),
           "--benchmark", benchmark,
           "--results-dir", results_dir,
           "--provider", provider,
           "--output", out_path]
    if use_llm:
        cmd.append("--use-llm")
    env = dict(os.environ)
    env["PYTHONIOENCODING"] = "utf-8"     # 上游 print emoji，GBK 控制台会炸
    try:
        r = subprocess.run(cmd, cwd=upstream, env=env,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if r.returncode != 0:
            tail = r.stdout.decode("utf-8", "replace")[-400:]
            print(f"  [x] {script} ({provider}) 退出码 {r.returncode}: {tail}")
            return False
        return True
    except Exception as e:
        print(f"  [x] {script} ({provider}) 起不来: {e}")
        return False


def mean_latency(results_dir: str, provider: str):
    """语音产物才有 perceived_total_latency（用户说完 → agent 开始出声）。文本模式恒为 None。"""
    vals = []
    for f in glob.glob(os.path.join(results_dir, "*", f"result_{provider}.json")):
        try:
            with open(f, "r", encoding="utf-8") as fp:
                d = json.load(fp)
        except Exception:
            continue
        for k in ("perceived_total_latency", "absolute_total_latency"):
            v = d.get(k)
            if isinstance(v, (int, float)):
                vals.append(float(v))
                break
    return sum(vals) / len(vals) if vals else None


def fmt(v, digits=3):
    if v is None:
        return "-"
    if isinstance(v, float) and abs(v) < 10:
        return f"{v:.{digits}f}"
    return str(v)


def main() -> int:
    ap = argparse.ArgumentParser(description="FDB-v3 结果 → 汇总表")
    ap.add_argument("--results-dir", default=r"C:\Users\7052\Desktop\tmp\fdb_v3_data_released")
    ap.add_argument("--upstream", default=DEFAULT_UPSTREAM)
    ap.add_argument("--benchmark", default=None,
                    help="缺省 <upstream>/benchmark_data_v2.json（评测脚本按它的 id 对回场景）")
    ap.add_argument("--providers", default="", help="逗号分隔；缺省自动发现")
    ap.add_argument("--min-count", type=int, default=50, help="样本数少于此的 provider 跳过（过滤调试产物）")
    ap.add_argument("--use-llm", action="store_true", help="开 gpt-4o judge（Resp Qual / 语义 Arg Acc）")
    ap.add_argument("--work-dir", default=None, help="评测报告落盘目录（缺省 <results-dir>/_reports）")
    ap.add_argument("--out", default=None, help="把 markdown 表写到这个文件")
    args = ap.parse_args()

    benchmark = args.benchmark or os.path.join(args.upstream, "benchmark_data_v2.json")
    # ⚠ 一律用绝对路径：上游脚本是 cwd=upstream 起的，相对 --output 会落到上游目录（甚至直接 FileNotFound）
    work_dir = os.path.abspath(args.work_dir or os.path.join(args.results_dir, "_reports"))
    results_dir = os.path.abspath(args.results_dir)
    os.makedirs(work_dir, exist_ok=True)

    counts = discover_providers(args.results_dir)
    if args.providers:
        providers = [p.strip() for p in args.providers.split(",") if p.strip()]
    else:
        providers = [p for p, n in sorted(counts.items(), key=lambda kv: -kv[1]) if n >= args.min_count]
    if not providers:
        print("[x] 没发现可评测的 provider")
        return 2

    print(f"[i] {args.results_dir}")
    print(f"[i] provider: {', '.join(f'{p}({counts.get(p, 0)})' for p in providers)}")
    print(f"[i] judge(use-llm): {'ON' if args.use_llm else 'OFF（Resp Qual 会空、Arg Acc 偏严）'}")

    rows = []
    for p in providers:
        ev = os.path.join(work_dir, f"{p}_evaluation_report.json")
        pr = os.path.join(work_dir, f"{p}_pass_rate_report.json")
        ok1 = run_upstream("evaluate_tool_calls.py", args.upstream, benchmark,
                           results_dir, p, ev, args.use_llm)
        ok2 = run_upstream("evaluate_pass_rate.py", args.upstream, benchmark,
                           results_dir, p, pr, args.use_llm)
        row = {"provider": p, "n": counts.get(p, 0),
               "tool_sel": None, "arg_acc": None, "resp_qual": None,
               "pass1": None, "take_turn": None, "latency": mean_latency(results_dir, p)}
        if ok1:
            with open(ev, "r", encoding="utf-8") as f:
                d = json.load(f)
            m = d.get("by_metric", {})
            row["tool_sel"] = m.get("tool_selection_acc")
            row["arg_acc"] = m.get("argument_acc")
            row["resp_qual"] = m.get("response_qual")
            row["take_turn"] = d.get("turn_taking", {}).get("turn_take_rate")
        if ok2:
            with open(pr, "r", encoding="utf-8") as f:
                row["pass1"] = json.load(f).get("overall_pass_rate")
        rows.append(row)
        print(f"  [ok] {p:<12} tool_sel={fmt(row['tool_sel'])} arg={fmt(row['arg_acc'])} "
              f"pass@1={fmt(row['pass1'])} take={fmt(row['take_turn'])}")

    # 按 Tool Sel 降序（缺值排最后），与论文表同样好读
    rows.sort(key=lambda r: (-(r["tool_sel"] or -1)))

    lines = ["| 模型 | Tool Sel↑ | Arg Acc↑ | Resp Qual↑ | Pass@1↑ | Take-turn↑ | Latency↓ | N |",
             "|---|---|---|---|---|---|---|---|"]
    for r in rows:
        lines.append("| {} | {} | {} | {} | {} | {} | {} | {} |".format(
            r["provider"], fmt(r["tool_sel"]), fmt(r["arg_acc"]),
            fmt(r["resp_qual"]) if r["resp_qual"] is not None else "-",
            fmt(r["pass1"]), fmt(r["take_turn"]),
            (fmt(r["latency"], 2) + "s") if r["latency"] is not None else "-",
            r["n"]))
    table = "\n".join(lines)
    print("\n" + table + "\n")
    if args.out:
        with open(args.out, "w", encoding="utf-8") as f:
            f.write(table + "\n")
        print(f"[ok] 表已写入 {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
