#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
net_sales_yoy_search 用例的后台服务（daemon）：一台"搜索引擎 + 数据站点"。

**为什么必须由 daemon 承担**
  web_search / web_fetch 的结果天然是"外面世界的实时内容"——直接拿真网测就永远判不了：
  内容每分钟在变、还依赖地区/网络/个性化，判据写不硬，还让整套回归依赖外网。而这一组用例要考的
  是"模型能不能搜到 -> 抓对页 -> 读出数 -> 算出来"这条**链路**。所以这里把"外面世界"搬进本机：

    · 搜索服务  GET /search?q=<词>       -> RSS（工具默认按 RSS/JSON 解析；见 JZWebSearchTool::parseSearch）
    · 数据站点  GET /report/2025|2026    -> 官方年度报告页（正文里有销量数字，"官方口径"）
                GET /report/2024         -> 上上年的旧报告（年份陷阱）
                GET /rumor               -> 论坛传闻页（**错**数字：抄它就错）
                GET /broken              -> 404（打不开的来源）

  并且每轮**随机生成**两年销量（产品名固定写在 prompt 里，数值只有本脚本知道）：
  真值只落在 daemon.json，verify 从那里读 —— 模型翻 prompt 也抄不到，必须真的联网取数。

**请求留痕**：每个请求都会追加一行到 hits.jsonl（含 path/query/UA）。verify 靠它判断
"搜索接口有没有被打过、两个报告页有没有被抓过"——这是"这条链真的走过"的证据
（注意：它只能证明"被访问过"，不能证明"用的是 web_search 工具"而不是 curl）。

退出：本进程由宿主 kill（Windows 下 JZProcess 的 Job Object 会带走子进程），无须自退。
"""
import argparse
import json
import os
import random
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# UTF-8 输出保险：Windows 下 python 默认按 locale(GBK) 编码 stdout
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

PRODUCT = "星河 X1 智能手表"      # 主题词（会写进 prompt，模型据此搜索）
LAST_YEAR = 2025
THIS_YEAR = 2026

# ---- 本轮的"外面世界"（真值，只在 daemon.json 里；每轮随机）----
LAST_SALES = 0        # 去年总销量（台）
THIS_SALES = 0        # 今年总销量（台）
YOY_PCT = 0.0         # 同比增长率（%，一位小数）= (今年-去年)/去年*100
RUMOR_SALES = 0       # 传闻帖里的错误数字
SALES_2024 = 0        # 2024 年（旧报告，另一个数字）

SEARCH_PORT = 0
SITE_PORT = 0
HITS_PATH = "hits.jsonl"

# 数据站点这一轮的随机路径前缀: /<PREFIX>/report/2026 …。
# 为什么要有它：报告页 URL 只该出现在**搜索结果**里。端口可以猜(3911)，路径不该能猜 ——
# 否则模型绕过 web_search 直接拼 URL 也能拿到数（那这条用例就测不到搜索了）。
PREFIX = "/d0"


def init_truth():
    """随机生成本轮真值：两年销量 + 由它们算出的同比（一位小数）。"""
    global LAST_SALES, THIS_SALES, YOY_PCT, RUMOR_SALES, SALES_2024
    LAST_SALES = random.randrange(120000, 800001, 1000)
    # 先随机一个"意图同比"，再折算成今年销量整数；判据用的同比**由两个整数反算**（自洽）
    want = round(random.uniform(-22.0, 48.0), 1)
    THIS_SALES = int(round(LAST_SALES * (1.0 + want / 100.0) / 1000.0)) * 1000
    if THIS_SALES <= 0:
        THIS_SALES = LAST_SALES + 1000
    YOY_PCT = round((THIS_SALES - LAST_SALES) * 100.0 / LAST_SALES, 1)
    # 干扰数据：传闻帖给一个明显不合理的数；2024 报告是另一年的数
    RUMOR_SALES = int(LAST_SALES * random.choice([0.31, 2.7]))
    SALES_2024 = int(LAST_SALES * random.choice([0.72, 0.86]))


def fmt(n):
    """1234567 -> '1,234,567'（真网页里的千分位写法）"""
    return format(int(n), ",")


def report_html(year, sales, note):
    return """<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8">
<title>%s 年度销售报告 | 星河官方数据中心</title>
<style>body{font:14px/1.7 sans-serif;margin:0}#hd{background:#123;color:#fff;padding:8px 16px}
main{padding:16px;max-width:720px}table{border-collapse:collapse}td,th{border:1px solid #ccc;padding:4px 8px}
footer{border-top:1px solid #ddd;color:#666;padding:8px 16px;font-size:12px}</style>
<script>window.__track=function(){return "analytics";};__track();</script>
</head><body>
<div id="hd">星河官方数据中心 · 年度报告</div>
<nav><a href="/">首页</a> | <a href="/report/%d">上一年度</a> | <a href="/about">关于我们</a></nav>
<main>
<h1>%s 年度销售报告</h1>
<p>本报告口径：全渠道销量，单位「台」，数据统计截至当年 12 月 31 日，已剔除退货与换货。</p>
<p><b>%s 年度总销量：%s 台</b></p>
<p>%s</p>
</main>
<footer>© 星河科技 · 本页数据仅用于内部经营分析 · 客服电话 400-000-0000
<script>/* footer script 也是噪声 */</script></footer>
</body></html>""" % (year, year - 1, year, year, fmt(sales), note)


def rumor_html():
    return """<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8">
<title>星河 X1 是不是要凉了？销量数据有点吓人 - 数码闲聊区</title>
<style>.post{border-bottom:1px solid #eee;padding:8px 0}</style>
<script>console.log("forum ad slot");</script>
</head><body>
<div class="topbar">数码闲聊区 &gt; 智能穿戴 &gt; 帖子</div>
<nav><a href="/">版块</a> <a href="/hot">热帖</a></nav>
<main>
<h1>星河 X1 是不是要凉了？销量数据有点吓人</h1>
<div class="post">楼主：我朋友在渠道那边，说今年出货只有 <b>%s</b> 台，跟去年比基本腰斩……</div>
<div class="post">2 楼：这种小道消息别当真，等官方口径。</div>
<div class="post">3 楼：+1，楼主删帖吧。</div>
</main>
<footer>本页为用户发言，不代表本站立场 · 举报请联系管理员</footer>
</body></html>""" % fmt(RUMOR_SALES)


def search_rss(query):
    """按"搜索引擎返回一页结果"的样子给 RSS：真两条 + 三个干扰源。
    ⚠ 摘要里**不写数字** —— 否则模型只看摘要就够了，用例就测不到 web_fetch。
    ⚠ 元素一律写成 <title>..</title> 这种无属性形式（工具用 <tag> 精确匹配解析）。"""
    site = "http://127.0.0.1:%d%s" % (SITE_PORT, PREFIX)
    items = [
        ("%s %d 年度销售报告 | 星河官方数据中心" % (PRODUCT, THIS_YEAR),
         "%s/report/%d" % (site, THIS_YEAR),
         "官方发布：全渠道销量、分季度明细与同比说明，点击查看完整报告正文。"),
        ("%s %d 年度销售报告 | 星河官方数据中心" % (PRODUCT, LAST_YEAR),
         "%s/report/%d" % (site, LAST_YEAR),
         "官方发布：上一年度全渠道销量与经营分析，点击查看完整报告正文。"),
        ("%s 是不是要凉了？销量数据有点吓人 - 数码闲聊区" % PRODUCT,
         "%s/rumor" % site,
         "论坛用户爆料，称今年出货量出现大幅变化，内容未经官方确认。"),
        ("%s %d 年度销售报告（历史归档）" % (PRODUCT, LAST_YEAR - 1),
         "%s/report/%d" % (site, LAST_YEAR - 1),
         "历史归档报告，请注意年份。"),
        ("%s 第三方销量数据汇总（含趋势图）" % PRODUCT,
         "%s/broken" % site,
         "第三方数据平台整理的汇总页，含近三年趋势。"),
    ]
    out = ['<?xml version="1.0" encoding="utf-8"?>', '<rss version="2.0"><channel>']
    out.append("<title>搜索：%s</title>" % query.replace("&", "&amp;").replace("<", "&lt;"))
    for title, link, desc in items:
        out.append("<item>")
        out.append("<title>%s</title>" % title.replace("&", "&amp;").replace("<", "&lt;"))
        out.append("<link>%s</link>" % link)
        out.append("<description>%s</description>" % desc)
        out.append("</item>")
    out.append("</channel></rss>")
    return "\n".join(out)


class Handler(BaseHTTPRequestHandler):
    server_version = "net_search_daemon/1.0"
    protocol_version = "HTTP/1.1"

    def _send(self, code, body, ctype):
        raw = body.encode("utf-8") if isinstance(body, str) else body
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(raw)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        try:
            self.wfile.write(raw)
        except Exception:
            pass

    def _hit(self, path, query):
        try:
            with open(HITS_PATH, "a", encoding="utf-8") as f:
                f.write(json.dumps({
                    "ts": int(time.time() * 1000),
                    "path": path,
                    "query": query,
                    "ua": self.headers.get("User-Agent", ""),
                }, ensure_ascii=False) + "\n")
                f.flush()
        except Exception:
            pass

    def do_GET(self):
        full = self.path
        path = full.split("?")[0]
        query = full.split("?", 1)[1] if "?" in full else ""
        self._hit(path, query)

        # 数据站点认随机前缀（/d0abc/report/2026）；也容忍无前缀访问，便于人工排错
        if PREFIX and path.startswith(PREFIX + "/"):
            path = path[len(PREFIX):]

        if path == "/search":
            # 搜索服务：无 q 也照常回一页（工具总会带 q），保持"永远有结果"
            self._send(200, search_rss(query), "application/rss+xml; charset=utf-8")
            return
        if path == "/report/%d" % THIS_YEAR:
            self._send(200, report_html(THIS_YEAR, THIS_SALES,
                                        "管理层点评：本年度销量同比变化已在下一年度报告说明。"),
                       "text/html; charset=utf-8")
            return
        if path == "/report/%d" % LAST_YEAR:
            self._send(200, report_html(LAST_YEAR, LAST_SALES,
                                        "管理层点评：本年度渠道结构以线上为主。"),
                       "text/html; charset=utf-8")
            return
        if path == "/report/%d" % (LAST_YEAR - 1):
            self._send(200, report_html(LAST_YEAR - 1, SALES_2024,
                                        "管理层点评：历史归档报告，仅作对比用。"),
                       "text/html; charset=utf-8")
            return
        if path == "/rumor":
            self._send(200, rumor_html(), "text/html; charset=utf-8")
            return
        if path == "/":
            self._send(200, "<!doctype html><html><body>数据站点首页（请从搜索结果进入报告页）</body></html>",
                       "text/html; charset=utf-8")
            return
        self._send(404, "<!doctype html><html><body>404 Not Found</body></html>",
                   "text/html; charset=utf-8")

    def log_message(self, fmt, *args):
        pass        # 静音访问日志，免得把管道塞满


def main():
    global SEARCH_PORT, SITE_PORT, HITS_PATH
    ap = argparse.ArgumentParser()
    ap.add_argument("--base-port", type=int, default=3910,
                    help="搜索服务用 N，数据站点用 N+1")
    ap.add_argument("--out", default="daemon.json", help="真值元数据写出路径")
    ap.add_argument("--seed", type=int, default=0, help="固定随机种子（复现某轮用；0=随机）")
    args = ap.parse_args()

    if args.seed:
        random.seed(args.seed)
    init_truth()

    # 随机路径前缀（每轮不同）：报告页 URL 只能在搜索结果里出现，不能被猜出来。
    global PREFIX
    PREFIX = "/d" + ("%06x" % random.randrange(1 << 24))

    SEARCH_PORT = args.base_port
    SITE_PORT = args.base_port + 1

    out_path = os.path.abspath(args.out)
    out_dir = os.path.dirname(out_path) or "."
    if not os.path.isdir(out_dir):
        os.makedirs(out_dir, exist_ok=True)
    HITS_PATH = os.path.join(out_dir, "hits.jsonl")
    with open(HITS_PATH, "w", encoding="utf-8"):     # 每轮清空：留痕只属于本轮
        pass

    site = "http://127.0.0.1:%d%s" % (SITE_PORT, PREFIX)
    meta = {
        "product": PRODUCT,
        "search_port": SEARCH_PORT,
        "site_port": SITE_PORT,
        "site_prefix": PREFIX,
        "search_url_prefix": "http://127.0.0.1:%d/search?q=" % SEARCH_PORT,
        "report_urls": {
            str(LAST_YEAR - 1): "%s/report/%d" % (site, LAST_YEAR - 1),
            str(LAST_YEAR): "%s/report/%d" % (site, LAST_YEAR),
            str(THIS_YEAR): "%s/report/%d" % (site, THIS_YEAR),
        },
        "rumor_url": "%s/rumor" % site,
        "broken_url": "%s/broken" % site,
        "last_year": LAST_YEAR,
        "this_year": THIS_YEAR,
        "last_sales": LAST_SALES,
        "this_sales": THIS_SALES,
        "expected_yoy": YOY_PCT,          # 判据：同比增长率（%，一位小数）
        "rumor_sales": RUMOR_SALES,       # 抄了传闻帖就会得到这个错值
        "sales_2024": SALES_2024,
        "hits_file": HITS_PATH,
    }
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(meta, f, ensure_ascii=False, indent=2)

    try:
        srv = ThreadingHTTPServer(("127.0.0.1", SEARCH_PORT), Handler)
        srv2 = ThreadingHTTPServer(("127.0.0.1", SITE_PORT), Handler)
    except Exception as e:
        print("daemon bind failed: %s" % e, flush=True)
        return 1

    threading.Thread(target=srv.serve_forever, daemon=True).start()
    threading.Thread(target=srv2.serve_forever, daemon=True).start()

    print("ready search=%d site=%d last=%d this=%d yoy=%.1f out=%s"
          % (SEARCH_PORT, SITE_PORT, LAST_SALES, THIS_SALES, YOY_PCT, out_path), flush=True)
    try:
        while True:
            time.sleep(0.5)
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
