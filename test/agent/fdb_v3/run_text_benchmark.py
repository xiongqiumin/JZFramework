#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
FDB-v3 文本模式的批量跑批器：把 benchmark_data_v2.json 的每个场景喂给 text_agent，
落盘成**上游评测脚本直接能吃的** result_{provider}.json。

为什么是这个落盘位置/字段：上游 evaluate_tool_calls.py / evaluate_pass_rate.py 用
    Path(results_dir).rglob(f"result_{provider}.json")
扫描结果，并按 result 里的 `example_id` 对回 benchmark 场景；再用
    result["actual_tool_calls"] / result["transcript"] / result["asr_chunks"]
三个字段做判据。所以本脚本只要把这三个字段填对，评测侧零改动。

输出目录：
    <out>/<provider>/<example_id>/result_<provider>.json

用法（本地内网端点）：
    python run_text_benchmark.py \
        --base-url http://192.168.111.123:8000/v1 \
        --api-key  <key> --model ugreen-ai-model --provider ugreen_text

    # 只跑几条
    python run_text_benchmark.py ... --limit 5
    python run_text_benchmark.py ... --ids travel_01,travel_02

然后用上游评测（在 fdb_v3_upstream 目录）：
    python evaluate_tool_calls.py --benchmark benchmark_data_v2.json \
        --results-dir <out>/ugreen_text --provider ugreen_text --output ugreen_eval.json
    python evaluate_pass_rate.py  --benchmark benchmark_data_v2.json \
        --results-dir <out>/ugreen_text --provider ugreen_text --output ugreen_pass.json
"""

import argparse
import datetime
import json
import os
import sys
import traceback
from concurrent.futures import ThreadPoolExecutor, as_completed

from text_agent import ChatClient, TextAgent, SYSTEM_PROMPT, load_mock_registry

DEFAULT_UPSTREAM = r"C:\Users\7052\Desktop\tmp\fdb_v3_upstream"


def load_scenarios(path: str):
    with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)
    if isinstance(data, dict) and "scenarios" in data:
        return data["scenarios"]
    return data


def load_data_dir(root: str):
    """真实数据目录：扫 <root>/*/metadata.json（100 个样本 / 79 个唯一 scenario）。

    ⚠ 与 benchmark_data_v2.json 的区别：metadata 里的 dialogue 只有**真实那 1 轮**
    （仓库场景表每场景有 2 轮，多一句寒暄热身）。期望工具调用两边一致。
    """
    import glob

    out = []
    for mp in sorted(glob.glob(os.path.join(root, "*", "metadata.json"))):
        with open(mp, "r", encoding="utf-8") as f:
            s = json.load(f)
        s["_sample_dir"] = os.path.dirname(mp)
        out.append(s)
    return out


def run_one(pid: int, scenario: dict, args, registry) -> dict:
    """跑单个场景：按 dialogue 的 user 文本逐回合喂，收集回复与工具调用。"""
    eid = scenario["id"]
    started = datetime.datetime.now()

    result = {
        "pid": pid,
        "example_id": eid,
        "category": scenario.get("domain", scenario.get("category", "unknown")),
        "title": scenario.get("title", ""),
        "difficulty": scenario.get("difficulty", ""),
        "provider": args.provider,
        "mode": "text",                      # 标明这是文本链路（无音频/无延迟指标）
        "evaluated_at": started.isoformat(),
    }

    # 每个场景一个独立会话（上游也是每条音频一个 room / 一次会话）
    client = ChatClient(base_url=args.base_url, api_key=args.api_key, model=args.model,
                        timeout=args.timeout, temperature=args.temperature)
    agent = TextAgent(client, registry, system_prompt=SYSTEM_PROMPT,
                      max_tool_rounds=args.max_tool_rounds)

    user_turns = [t.get("user") or t.get("user_annotated") or "" for t in scenario.get("dialogue", [])]
    turns = []
    replies = []
    llm_ms = 0.0
    try:
        for i, utext in enumerate(user_turns):
            if not utext.strip():
                continue
            out = agent.send(utext)
            llm_ms += out["llm_ms"]
            replies.append(out["reply"])
            turns.append({
                "turn": i + 1,
                "user": utext,
                "assistant": out["reply"],
                "calls": [c["function"] for c in out["calls"]],
                "llm_rounds": out["rounds"],
            })
        result["status"] = "completed"
    except Exception as e:                    # 端点/网络问题：也落盘，评测侧当 0 分，便于事后统计
        result["status"] = "error"
        result["error"] = f"{type(e).__name__}: {e}"
        result["traceback"] = traceback.format_exc()[-1500:]
        print(f"  [x] {eid}: {result['error']}")

    # ---- 与上游 result_{provider}.json 对齐的三个关键字段 ----
    result["input_transcript"] = "\n".join(t for t in user_turns if t.strip())
    result["transcript"] = " ".join(r for r in replies if r.strip())   # 上游是整段输出音频的 ASR 全文
    result["asr_chunks"] = []                                          # 文本模式没有音频分片
    result["actual_tool_calls"] = agent.calls

    # ---- 附加信息（评测脚本不读，排查/复现用）----
    result["turns"] = turns
    result["usage"] = agent.usage_total
    result["llm_elapsed_s"] = round(llm_ms / 1000.0, 3)
    result["total_elapsed_s"] = round((datetime.datetime.now() - started).total_seconds(), 3)
    result["_sample_dir"] = scenario.get("_sample_dir", "")   # 数据目录模式：结果写回样本目录
    return result


def main() -> int:
    ap = argparse.ArgumentParser(description="FDB-v3 文本模式跑批（输入文本 / 输出文本）")
    ap.add_argument("--benchmark", default=None,
                    help="benchmark_data_v2.json 路径（缺省 <upstream>/benchmark_data_v2.json）")
    ap.add_argument("--data-dir", default="",
                    help="真实数据目录（fdb_v3_data_released）；给了就按样本目录跑，结果写回样本目录")
    ap.add_argument("--upstream", default=DEFAULT_UPSTREAM,
                    help="fdb_v3_upstream 目录（用于 import mock_apis 与默认 benchmark）")
    ap.add_argument("--base-url", default=os.environ.get("OPENAI_BASE_URL", "https://api.openai.com/v1"))
    ap.add_argument("--api-key", default=os.environ.get("OPENAI_API_KEY", ""))
    ap.add_argument("--model", default=os.environ.get("FDB_TEXT_MODEL", "gpt-4o"))
    ap.add_argument("--provider", default="cascaded_text",
                    help="结果文件名/目录名里用的 provider 标识（也是评测脚本的 --provider）")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "out"))
    ap.add_argument("--limit", type=int, default=0, help="只跑前 N 条（0=全部）")
    ap.add_argument("--ids", default="", help="逗号分隔的场景 id，只跑这些")
    ap.add_argument("--workers", type=int, default=4, help="并发场景数（每个场景一个独立会话）")
    ap.add_argument("--max-tool-rounds", type=int, default=6, help="单回合内最多允许的模型往返次数")
    ap.add_argument("--temperature", type=float, default=None, help="缺省不传（用服务端默认）")
    ap.add_argument("--timeout", type=float, default=180.0, help="单次 HTTP 超时（秒）")
    ap.add_argument("--latency-profile", default="instant",
                    help="mock API 延迟档（instant/fast/normal/slow）；文本评测建议 instant")
    ap.add_argument("--skip-existing", action="store_true", help="已有 result 文件的场景跳过")
    args = ap.parse_args()

    benchmark = args.benchmark or os.path.join(args.upstream, "benchmark_data_v2.json")
    scenarios = load_data_dir(args.data_dir) if args.data_dir else load_scenarios(benchmark)

    if args.ids:
        want = {s.strip() for s in args.ids.split(",") if s.strip()}
        scenarios = [s for s in scenarios
                     if s["id"] in want or os.path.basename(s.get("_sample_dir", "")) in want]
        if not scenarios:
            print(f"[x] --ids 没匹配到任何场景: {args.ids}")
            return 2
    if args.limit > 0:
        scenarios = scenarios[: args.limit]

    registry = load_mock_registry(args.upstream, latency_profile=args.latency_profile)

    out_root = os.path.join(args.out, args.provider)
    if not args.data_dir:
        os.makedirs(out_root, exist_ok=True)

    def result_dir(sc):
        """数据目录模式写回样本目录；场景表模式写 <out>/<provider>/<id>/"""
        return sc.get("_sample_dir") or os.path.join(out_root, sc["id"])

    todo = []
    for pid, sc in enumerate(scenarios):
        if args.skip_existing and os.path.isfile(
                os.path.join(result_dir(sc), f"result_{args.provider}.json")):
            continue
        todo.append((pid, sc))

    print(f"[i] 场景 {len(scenarios)} 条（待跑 {len(todo)}），模型 {args.model} @ {args.base_url}")
    print(f"[i] 输出 {out_root}")

    done = 0
    results = []
    with ThreadPoolExecutor(max_workers=max(1, args.workers)) as pool:
        futures = {pool.submit(run_one, pid, sc, args, registry): sc for pid, sc in todo}
        for fut in as_completed(futures):
            sc = futures[fut]
            try:
                r = fut.result()
            except Exception as e:            # 兜底，别让一条炸掉整批
                r = {"pid": 0, "example_id": sc["id"], "provider": args.provider,
                     "status": "error", "error": f"{type(e).__name__}: {e}",
                     "actual_tool_calls": [], "transcript": "", "asr_chunks": []}
            d = r.pop("_sample_dir", "") or os.path.join(out_root, r["example_id"])
            os.makedirs(d, exist_ok=True)
            with open(os.path.join(d, f"result_{args.provider}.json"), "w", encoding="utf-8") as f:
                json.dump(r, f, indent=2, ensure_ascii=False)
            results.append(r)
            done += 1
            names = [c["function"] for c in r.get("actual_tool_calls", [])]
            label = os.path.basename(d) if args.data_dir else r["example_id"]
            print(f"  [{done}/{len(todo)}] {label:<40} {r.get('status'):<9} "
                  f"tools={names} turns={len(r.get('turns', []))} {r.get('total_elapsed_s', 0)}s")

    # ---- 轻量汇总（不是上游评测，只是跑完能一眼看数量/失败原因）----
    ok = sum(1 for r in results if r.get("status") == "completed")
    summary = {
        "provider": args.provider,
        "model": args.model,
        "base_url": args.base_url,
        "mode": "text",
        "finished_at": datetime.datetime.now().isoformat(),
        "total": len(results),
        "completed": ok,
        "errors": len(results) - ok,
        "tool_calls_total": sum(len(r.get("actual_tool_calls", [])) for r in results),
        "prompt_tokens": sum(r.get("usage", {}).get("prompt_tokens", 0) for r in results),
        "completion_tokens": sum(r.get("usage", {}).get("completion_tokens", 0) for r in results),
    }
    os.makedirs(out_root, exist_ok=True)
    with open(os.path.join(out_root, f"run_summary_{args.provider}.json"), "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=2, ensure_ascii=False)
    print(f"[ok] 完成 {ok}/{len(results)}，工具调用共 {summary['tool_calls_total']} 次，"
          f"token prompt={summary['prompt_tokens']} completion={summary['completion_tokens']}")
    print(f"   汇总: {os.path.join(out_root, f'run_summary_{args.provider}.json')}")
    return 0 if ok == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
