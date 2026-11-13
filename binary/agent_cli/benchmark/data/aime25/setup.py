#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
aime25 评测集的生成器（"加载框架"）：把下载到的 lm-evaluation-harness 原始结果文件
（aime25.jsonl，每行一个 doc，含 doc.problem / doc.answer / resps / exact_match …）转成
benchmark 能直接加载的用例 JSON（data/aime25/aime25_<NN>.json —— 文件名与用例 name 一致）。

为什么需要转换：
- benchmark 的"当前支持的格式"是**每个用例一个 JSON 对象**（name / prompt / maxAttempts /
  timeout / expect.contains / expect.equals / verify … 见
  binary/code_agent/benchmark/main.cpp 的 parseCaseObject），
  目录模式会扫描 *.json，或者用同目录的 cases.jsonl 索引。
- aime25.jsonl 是 lm-eval 的输出格式（doc_id / doc / target / arguments / resps / filtered_resps /
  *_hash / exact_match），既不是用例对象，也不是"指向用例文件的索引行"，直接喂进 benchmark 会被逐行
  当作索引项解析（没有 file 字段）而全部跳过。

转换口径：
- 题目文本取 doc.problem（lm-eval 的 "Question: …\\nAnswer:" 只是对话模板，这里不要）；
- 期望答案取 doc.answer（缺省回落顶层 target），AIME 答案恒为 0~999 的整数；
- 让模型写 TypeScript 算出答案，**直接把答案整数打印出来**（不要 `\boxed{}` 之类的格式符号）；
- 判定用**现成的** `expect.equals`：产物的规范化输出（CRLF→LF + 去首尾空白）必须**完全等于答案整数本身**
  （例如 `60`）。⇒ 所以 prompt 把输出契约定死成"标准输出只能有这个数字"（两边必须自洽：措辞含糊会让模型多打印
  而丢分，2026-10-08 实测过 `只打印这一行作为最终结果` 这种说法挡不住过程/自检行）；
  不用 `contains`（"出现过就算对"太宽，且 `60` 会被 `160`/`600` 子串命中）；
- 期望答案不写进 prompt/工程，只在用例 JSON 的 expect 里，模型看不到；
- `maxAttempts = 1` = 只跑一次、不重试（失败即记 FAIL）；AIME 要的是"一次给出答案"。
- 同时被 code_agent_benchmark（宿主内联 check）与 agent_cli_test / test_codex_bench（自动挂
  data/stdout_verify.py 通用验证）复用，无需额外脚本。

运行：在 data/aime25/ 下 `python setup.py`（生成后需保证 data/cases.jsonl 里有对应
`{"file": "aime25/aime25_<NN>.json"}` 条目；本脚本末尾会打印出应有的索引行）。
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.join(HERE, "aime25.jsonl")   # lm-eval 原始数据（只读，保留作溯源）

EVAL_CONTRACT = (
    "用 TypeScript 解一道数学竞赛题（AIME）。\n"
    "工程骨架已就位：入口 src/main.ts（编译产物 build/main.js）；tsconfig.json 已配好，不要改它、也不要另建工程。\n"
    "\n"
    "要求：\n"
    "1) 在 src/main.ts 里写代码（只用 TS），把下面这道题的答案算出来；\n"
    "2) 程序结束前把答案打印出来：一个 0~999 的整数，**只要这个数字本身**：\n"
    "   不要任何其它内容（过程、中间量、调试、自检都不要）\n"
    "   例如答案是 70，程序的标准输出就只能是 70 这一行。\n"
    "   判定是【产物整段输出与这个数字完全相等】，多打印任何内容都会被判为未通过。\n"
    "\n"
    "题目：\n"
)

RETRY_HINT = (
    "上一次运行未通过：程序的标准输出不是恰好那一个答案整数。注意输出格式, 只打印结果不要打印任何其它内容"
)


def read_source(path):
    """读 lm-eval 原始 jsonl（每行一个 JSON 对象），返回记录列表。"""
    rows = []
    with open(path, encoding="utf-8") as f:
        for line_no, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except ValueError as e:
                print("[setup] 第 %d 行解析失败，跳过: %s" % (line_no, e))
                continue
            rows.append(obj)
    return rows


def extract(record):
    """从一条 lm-eval 记录里取出 (doc_id, problem, answer)；取不到返回 None。"""
    doc = record.get("doc") if isinstance(record.get("doc"), dict) else {}
    problem = doc.get("problem")
    answer = doc.get("answer", record.get("target"))
    if not isinstance(problem, str) or not problem.strip():
        return None
    if answer is None:
        return None
    answer = str(answer).strip()
    if not answer:
        return None
    doc_id = doc.get("id", record.get("doc_id"))
    return doc_id, problem.strip(), answer


def build_case_json(doc_id, problem, answer, index):
    idx = "%02d" % index
    return {
        "name": "aime25_%s" % idx,
        "purpose": "AIME 2025 数学题（源 doc_id=%s）。" % str(doc_id),
        "prompt": EVAL_CONTRACT + problem,
        # maxAttempts = **总尝试次数（含第一次）** ⇒ 1 = 只跑一次、不重试（失败即记 FAIL）。
        "maxAttempts": 1,
        "timeout": 60,
        # 判定: 产物的规范化输出必须完全等于答案整数本身（用现成的 expect.equals, 不新增 expect 分类）。
        "expect": {
            "equals": answer
        },
        "retryHint": RETRY_HINT,
    }


def main():
    if not os.path.isfile(SOURCE):
        print("[setup] 找不到原始数据: %s" % SOURCE)
        return 1

    # 清掉旧的用例 JSON（aime25_*.json；gen_*.json 是历史命名），重跑幂等。
    # ⚠ 只删 .json，别碰 aime25.jsonl（原始数据）与 setup.py。
    for fn in os.listdir(HERE):
        if (fn.startswith("aime25_") or fn.startswith("gen_")) and fn.endswith(".json"):
            try:
                os.remove(os.path.join(HERE, fn))
            except Exception:
                pass

    rows = read_source(SOURCE)
    count = 0
    index_lines = []
    for record in rows:
        got = extract(record)
        if got is None:
            print("[setup] 跳过缺 problem/answer 的记录: doc_id=%s" % record.get("doc_id"))
            continue
        doc_id, problem, answer = got
        case = build_case_json(doc_id, problem, answer, count)
        out = os.path.join(HERE, "aime25_%02d.json" % count)
        with open(out, "w", encoding="utf-8", newline="\n") as f:
            json.dump(case, f, ensure_ascii=False, indent=2)
        index_lines.append('{"file": "aime25/aime25_%02d.json"}' % count)
        count += 1
        print("[setup] 生成用例: %s (doc_id=%s, 答案=%s)" % (out, doc_id, answer))

    print("[setup] 完成，共生成 %d 个用例。" % count)
    print("[setup] data/cases.jsonl 里应有以下 %d 行：" % count)
    for line in index_lines:
        print("  " + line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
