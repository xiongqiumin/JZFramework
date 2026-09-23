#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
algorithms 评测集的生成器（"加载框架"）：

- cases.txt：一行一个算法用例，格式 `id|标题|难度`（仅 id 也行）。
- 本脚本读取 cases.txt，为每个启用的题目生成一个 benchmark 可直接加载的用例 JSON
  （data/algorithms/gen_<id>.json），其 setup 指向同目录 algo_setup.py、verify 指向 algo_verify.py。
- algo_setup.py / algo_verify.py 是同目录提交的"测试框架"：algo_setup 把测试输入（testdata.json）
  写进工程并注入评测脚手架；algo_verify 启动模型产物并用参考实现重算正确输出、比对 agent 写出的 result.json。

框架模式（与 net_* 一致）：
- 注入的 src/main.ts（不可改）负责读取 testdata.json、对每个输入调用 solve(input)、写出 result.json；
- 模型只需在 src/solution.ts 实现 `export function solve(input: any): any`（单个输入 -> 单个输出）；
- 模型不读写任何文件、不处理输入，"单纯测算法能力"。

运行：在 data/algorithms/ 下 `python setup.py`。
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

# ---- 题目注册表：id -> 元信息 + 测试输入生成 + 题目文本（描述 solve(input) 的语义）----
def _in_bubble():
    return [[3, 1, 2], [5, 4, 3, 2, 1], [], [1], [2, 2, 1, 3], [9, 3, 7, 1, 8, 5], [-1, -3, -2], [0]]

def _in_reverse():
    return [[1, 2, 3], [5, 4, 3], [], [1], [1, 1, 1, 2], [7, 8, 9, 10], [-1, 2, -3]]

def _in_bsearch():
    return [[[1, 2, 3, 4, 5], 3], [[1, 2, 3, 4, 5], 1], [[1, 2, 3, 4, 5], 5],
            [[1, 2, 3, 4, 5], 6], [[], 1], [[1], 1], [[2, 4, 6, 8, 10], 8], [[2, 4, 6, 8, 10], 3]]

def _in_twosum():
    return [[[2, 7, 11, 15], 9], [[3, 2, 4], 6], [[3, 3], 6], [[1, 2], 10],
            [[0, 4, 3, 0], 0], [[5, 5, 5, 5], 10], [[-1, -2, -3, -4], -6]]

def _in_fib():
    return [0, 1, 2, 5, 10, 20, 1, 3]

def _in_pal():
    return ["aba", "abba", "abc", "a", "ab", "", "racecar", "hello"]

def _in_kadane():
    return [[-2, 1, -3, 4, -1, 2, 1, -5, 4], [1, 2, 3], [-1, -2, -3],
            [5, -1, 3, -2, 4], [], [2, -1, 2, 3], [-5, -4, -3, -2]]

def _in_gcd():
    return [[12, 18], [17, 5], [0, 5], [48, 36], [100, 10], [7, 0], [21, 14], [123, 456]]

PROBLEMS = {
    "bubble_sort": {
        "title": "冒泡排序", "difficulty": "easy", "gen": _in_bubble,
        "text": "solve(input) 接收一个整数数组 input，返回其升序排序后的数组（稳定排序、按元素值比较）。",
    },
    "reverse_array": {
        "title": "数组反转", "difficulty": "easy", "gen": _in_reverse,
        "text": "solve(input) 接收一个数组 input，返回其反转后的数组（第一个元素变最后一个）。",
    },
    "binary_search": {
        "title": "二分查找", "difficulty": "medium", "gen": _in_bsearch,
        "text": "solve(input) 接收 [sortedArray, target]（sortedArray 是已升序的整型数组，target 为整数），"
                "返回 target 在 sortedArray 中的下标；不存在则返回 -1。",
    },
    "two_sum": {
        "title": "两数之和", "difficulty": "easy", "gen": _in_twosum,
        "text": "solve(input) 接收 [nums, target]（nums 为整型数组），返回任意一对满足 nums[i]+nums[j]==target "
                "且 i<j 的下标 [i,j]；若有多对返回 i 最小的一对、i 相同时返回 j 最小的一对；无解返回 [-1,-1]。",
    },
    "fib": {
        "title": "斐波那契", "difficulty": "easy", "gen": _in_fib,
        "text": "solve(input) 接收一个整数 n（n>=0），返回斐波那契数列第 n 项（fib(0)=0, fib(1)=1, fib(2)=1, fib(3)=2 ...）。",
    },
    "is_palindrome": {
        "title": "回文判断", "difficulty": "easy", "gen": _in_pal,
        "text": "solve(input) 接收一个字符串 input，若是回文（正读反读相同）返回 true，否则返回 false。",
    },
    "max_subarray": {
        "title": "最大子数组和(Kadane)", "difficulty": "medium", "gen": _in_kadane,
        "text": "solve(input) 接收一个整数数组 input，返回其和最大的连续子数组的和（空数组返回 0）。",
    },
    "gcd": {
        "title": "最大公约数", "difficulty": "easy", "gen": _in_gcd,
        "text": "solve(input) 接收 [a,b] 两个整数，返回 a 与 b 的最大公约数（gcd(x,0)=|x|）。",
    },
}

IO_CONTRACT = (
    "你要在工程里实现一道算法题。框架约定（务必遵守）：\n"
    "- 工程已提供 src/main.ts（评测框架，**禁止修改**）：它负责读取测试数据、调用你的函数、写出结果。\n"
    "- 你只需在 src/solution.ts 中实现：\n"
    "    export function solve(input: any): any\n"
    "  框架会读取测试数据里的 inputs（每个用例一个输入），对每一项调用 solve(input)，把返回值按顺序收集并写入 "
    "./result.json 的 outputs 数组（结构严格为 {\"outputs\":[...]}）。\n"
    "- 你只需实现“单个输入 -> 单个输出”的映射，不需要读取或写出任何文件，也不要改动 main.ts。\n"
    "- 框架会按参考实现重算每个用例的正确输出，并与你的 ./result.json 逐项精确比对；全部一致才通过。\n\n"
    "具体题目（你的 solve 应实现）：\n"
)


def parse_cases_txt(path):
    rows = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            pid = line.split("|")[0].strip()
            if pid:
                rows.append(pid)
    return rows


def build_case_json(pid, meta, inputs):
    prompt = IO_CONTRACT + meta["text"]
    return {
        "name": "algo_" + pid,
        "purpose": "基础算法/数据结构：%s（%s）。用真实输入/输出比对评测（框架读输入+写结果，模型只实现 solve），而非 md5。"
                   % (meta["title"], meta["difficulty"]),
        "prompt": prompt,
        "maxAttempts": 3,
        "timeout": 30,
        "setup": "algo_setup.py",
        # 只传题目 id（简单字符串，避免 --input 经 cmd 转义带引号 JSON 出问题）；
        # 真正的测试输入由 setup 按 id 从同目录 inputs/<id>.json 读取并写进工程。
        "input": pid,
        "verify": "python ${data}/algorithms/algo_verify.py --runtime ${runtime} --build ${build} --root ${root} --problem "
                  + pid,
        "retryHint": "上一次运行未通过输出比对。请确认：1) 在 src/solution.ts 实现 solve(input)（不要改 main.ts、不要读写文件）；"
                     "2) 按题目 %s 的语义对“单个输入”计算并用 return 返回结果；"
                     "3) 框架会自动收集所有用例的返回值并写入 result.json 的 outputs 数组，你无需处理多用例循环。"
                     % pid,
    }


def main():
    cases_txt = os.path.join(HERE, "cases.txt")
    if not os.path.isfile(cases_txt):
        print("[setup] 找不到 cases.txt: %s" % cases_txt)
        return 1
    pids = parse_cases_txt(cases_txt)

    # 清掉旧的 gen_*.json
    for fn in os.listdir(HERE):
        if fn.startswith("gen_") and fn.endswith(".json"):
            try:
                os.remove(os.path.join(HERE, fn))
            except Exception:
                pass

    count = 0
    inputs_dir = os.path.join(HERE, "inputs")
    os.makedirs(inputs_dir, exist_ok=True)
    for pid in pids:
        if pid not in PROBLEMS:
            print("[setup] 跳过未知题目: %s" % pid)
            continue
        meta = PROBLEMS[pid]
        inputs = meta["gen"]()
        # 把测试输入落盘到 inputs/<id>.json，供 algo_setup 按 id 读取
        with open(os.path.join(inputs_dir, pid + ".json"), "w", encoding="utf-8") as f:
            json.dump({"problem": pid, "inputs": inputs}, f, ensure_ascii=False)
        case = build_case_json(pid, meta, inputs)
        out = os.path.join(HERE, "gen_%s.json" % pid)
        with open(out, "w", encoding="utf-8") as f:
            json.dump(case, f, ensure_ascii=False, indent=2)
        count += 1
        print("[setup] 生成用例: %s (%d 个测试输入)" % (out, len(inputs)))
    print("[setup] 完成，共生成 %d 个用例。" % count)
    return 0


if __name__ == "__main__":
    sys.exit(main())
