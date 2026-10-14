#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
net_sales_yoy_search 用例的外部验证脚本。

被测链路（这是"搜索类"用例的判据骨架）：
  1) 模型必须用 **web_search** 去搜主题词 —— 搜索结果里才有那两个数据页的 URL（prompt 里没有）；
  2) 用 **web_fetch** 抓两个**官方年度报告**页的正文，读出两年销量；
  3) 算出同比增长率，并写一个 TS 程序把它打印出来（契约行 `YOY=<一位小数的百分数>`）。

判据来源：daemon.json（daemon 随机生成的两年销量与由它们反算的同比），**不写在 prompt 里**。
另外读 daemon 写的 hits.jsonl 做"链路留痕"断言：
  · 搜索接口被打过 -> 证明 web_search 真的用了本用例配置的搜索源
    （模型拿不到搜索 URL —— 它只存在于工具的配置里；连那两个报告页的 URL 也只能从搜索结果得到）
  · 两个官方报告页都被抓过 -> 证明 web_fetch 真的读了正文（光看搜索摘要读不到数字）

退出码：0=通过，1=未通过（stdout 作为重试反馈）。
⚠ 反馈里**不打印真值**，免得"重试"变成"抄答案"。因此本用例的 maxAttempts 设为 1
  （要改大就得先把下面那条 [info] 去掉）。
"""
import argparse
import json
import os
import re
import subprocess
import sys
from collections import Counter

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

YOY_RE = re.compile(r"YOY\s*[=:：]\s*(-?\d+(?:\.\d+)?)", re.IGNORECASE)
RUN_TIMEOUT = 40


def load_json(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def read_hits(path):
    rows = []
    if not path or not os.path.isfile(path):
        return rows
    try:
        with open(path, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    rows.append(json.loads(line))
                except Exception:
                    pass
    except Exception:
        pass
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runtime", required=True, help="code_runtime 可执行文件路径")
    ap.add_argument("--build", required=True, help="模型产物构建目录（运行时 cwd）")
    ap.add_argument("--root", default="", help="用例工程根目录（daemon.json 在这里）")
    ap.add_argument("--daemon-file", default="")
    ap.add_argument("--tolerance", type=float, default=0.06, help="同比数值容差（一位小数四舍五入）")
    args = ap.parse_args()

    build = os.path.normpath(args.build)
    root = os.path.normpath(args.root) if args.root else ""
    meta_path = args.daemon_file or (os.path.join(root, "daemon.json") if root else "daemon.json")

    if not os.path.isfile(meta_path):
        print("daemon 元数据不存在: %s（daemon 没起来？）" % meta_path)
        return 1
    try:
        meta = load_json(meta_path)
    except Exception as e:
        print("读取 daemon 元数据失败: %s: %s" % (meta_path, e))
        return 1

    expect_yoy = float(meta.get("expected_yoy", 0.0))
    last_year = meta.get("last_year")
    this_year = meta.get("this_year")
    last_url = (meta.get("report_urls") or {}).get(str(last_year), "")
    this_url = (meta.get("report_urls") or {}).get(str(this_year), "")
    hits = read_hits(meta.get("hits_file", ""))

    # ---- 1) 跑模型产物（纯计算+打印，一次性的）----
    js_out = ""
    try:
        proc = subprocess.Popen([args.runtime], cwd=build,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    except Exception as e:
        print("无法启动 code_runtime: %s" % e)
        return 1
    try:
        out, _ = proc.communicate(timeout=RUN_TIMEOUT)
        js_out = out.decode("utf-8", "replace") if out else ""
    except Exception:
        try:
            proc.kill()
        except Exception:
            pass
        print("code_runtime 超时(%ds)未收工（检查有没有挂住的定时器/句柄）" % RUN_TIMEOUT)
        if js_out.strip():
            print(js_out)
        return 1

    results = []

    def check(name, ok, detail=""):
        results.append((name, ok, detail))
        print("[%s] %s%s" % ("PASS" if ok else "FAIL", name, (": " + detail) if detail else ""))

    if proc.returncode != 0:
        print("[CRASH] code_runtime 退出码 %d" % proc.returncode)
        if js_out.strip():
            print(js_out)
        return 1
    check("program_exit", True, "退出码 0")

    # ---- 2) 契约行：YOY=<值> ----
    got_yoy = None
    m = None
    for m2 in YOY_RE.finditer(js_out):
        m = m2          # 取最后一个（前面可能有自检输出）
    if m:
        try:
            got_yoy = float(m.group(1))
        except Exception:
            got_yoy = None
    if got_yoy is None:
        check("yoy_line", False, "标准输出里没找到契约行 YOY=<同比>；实际输出=%r"
              % (js_out.strip()[:200]))
    else:
        check("yoy_line", True, "YOY=%.4g" % got_yoy)
        diff = abs(got_yoy - expect_yoy)
        check("yoy_value", diff <= args.tolerance,
              "算出的同比=%.4g（要求与官方两年数据算出的值一致，容差 %.2f）" % (got_yoy, args.tolerance))

    # ---- 3) 链路留痕：搜索接口被打过 / 两个官方报告页被抓过 ----
    search_hits = [h for h in hits if str(h.get("path", "")) == "/search"]
    check("web_search_used", len(search_hits) > 0,
          "搜索接口被访问 %d 次" % len(search_hits)
          + ("" if search_hits else "（搜索源没被打过 —— 检查本用例的 webSearchUrl 有没有生效）"))

    def hit_count(url):
        # http://127.0.0.1:3911/report/2025 -> /report/2025（hits.jsonl 里的 path 就是它）
        if not url:
            return 0
        path = re.sub(r"^[a-zA-Z]+://[^/]+", "", url)
        return sum(1 for h in hits if str(h.get("path", "")) == path)

    c_last, c_this = hit_count(last_url), hit_count(this_url)
    check("fetched_both_reports", c_last > 0 and c_this > 0,
          "官方报告页抓取次数: %s 年=%d, %s 年=%d（两年都要抓正文才读得到数字）"
          % (last_year, c_last, this_year, c_this))

    # ---- 汇总 ----
    failed = [r for r in results if not r[1]]
    if not results:
        print("RESULT: FAIL (无有效断言)")
        return 1
    if failed:
        print("RESULT: FAIL (%d/%d 失败)" % (len(failed), len(results)))
        tried = Counter(str(h.get("path", "")) for h in hits)
        print("---- daemon 收到的请求 ----")
        print(", ".join("%s x%d" % (k, v) for k, v in sorted(tried.items())) or "(无)")
        if js_out.strip():
            print("---- 程序输出 ----")
            print(js_out.strip()[:2000])
        print("---- 提示 ----")
        if not search_hits:
            print("· 没有访问搜索接口：必须用 web_search 工具搜主题词（报告页 URL 只能从搜索结果里拿）。")
        if not (c_last > 0 and c_this > 0):
            print("· 官方报告页没抓全：搜索结果里带「星河官方数据中心」的那两条才是官方口径，"
                  "要用 web_fetch 抓它们的正文；论坛帖/历史归档/打不开的链接都不算。")
        if got_yoy is not None and abs(got_yoy - expect_yoy) > args.tolerance:
            print("· 同比算错了：只认两个官方报告页正文里的「年度总销量：<数字> 台」，"
                  "同比 = (今年 - 去年) / 去年 * 100，保留一位小数。")
        return 1

    print("RESULT: PASS (%d 断言全部通过)" % len(results))
    # ⚠ 真值留在这里只为人工排错；若把本用例的 maxAttempts 改 >1，这行就会把答案回给模型。
    print("[info] daemon 真值: %s 年=%d 台, %s 年=%d 台, 同比=%.1f%%"
          % (last_year, meta.get("last_sales", 0), this_year, meta.get("this_sales", 0), expect_yoy))
    return 0


if __name__ == "__main__":
    sys.exit(main())
