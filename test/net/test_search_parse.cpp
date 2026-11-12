// ---------------------------------------------------------------------------
// 必应网页搜索结果（HTML）解析测试 —— 纯离线，不联网、不起服务
//
// 为什么单独测: web_search 的默认源已从 Bing RSS 改成 Bing HTML 结果页
// （RSS 通道实测已降级：搜"冬瓜"返回一堆 Telegram 结果）。HTML 解析一旦悄悄坏掉，
// 表现是"搜索一律无结果"，而模型侧看不出来 —— 必须有离线回归。
//
// 覆盖点:
//   1) <li class="b_algo"> 分块：标题 / 直链 / 摘要；
//   2) 标题里的 <strong> 等内联标签要清掉；
//   3) 摘要里的 &ensp;&#0183;&ensp; 要还原成 "·"，不能把实体漏给模型；
//   4) www.bing.com/ck/a?...&u=a1<base64url> 跳转链接要**本地解码**成真实 URL
//      （对比搜狗 /link?url=：那个必须再发一次请求）；
//   5) 缺 <p> 的条目仍能出标题+链接；
//   6) 非结果块（b_ans 等）不能被误收；
//   7) 验证页/空页面返回 false，别把垃圾当结果；
//   8) 相关性判定：结果与查询零交集 → 判源降级（web_search 据此直接回"搜索不可用"）。
//
// 期望输出: ALL PASS，退出码 0；有失败时打印 [FAIL] 并返回 1。
// ---------------------------------------------------------------------------
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "JZCodeTools.h"

using namespace std;

static int g_fail = 0;

static bool contains(const string &s, const string &sub)
{
    return s.find(sub) != string::npos;
}

static void check(bool ok, const string &what)
{
    if (!ok)
        ++g_fail;
    printf("%s %s\n", ok ? "[OK]  " : "[FAIL]", what.c_str());
}

// 贴近 cn.bing.com 真实结构（2026-11 抓取样本，已精简）
static const char *kBingPage =
    "<html><head><title>冬瓜 - 搜索</title></head><body>"
    "<ol id=\"b_results\">"
    // ① 直链 + 标题里带 <strong> + 摘要带 "·" 实体
    "<li class=\"b_algo\" data-id=\"\">"
    "<h2><a target=\"_blank\" href=\"https://baike.baidu.com/item/%E5%86%AC%E7%93%9C/422112\" "
    "h=\"ID=SERP,5146.1\"><strong>冬瓜</strong>（葫芦科冬瓜属植物）_百度百科</a></h2>"
    "<div class=\"b_caption\"><p class=\"b_lineclamp2 b_algoSlug\">"
    "2016年9月23日&ensp;&#0183;&ensp;冬瓜（学名：Benincasa hispida）是葫芦科冬瓜属植物。</p></div>"
    "</li>"
    // ② ck/a 跳转包裹（u=a1<base64url> 里是真实地址）
    "<li class=\"b_algo\">"
    "<h2><a href=\"https://www.bing.com/ck/a?!&amp;&amp;p=abc123&amp;"
    "u=a1aHR0cHM6Ly96aG9uZ3lpYmFpa2UuY29tL3dpa2kvJUU1JTg2JUFDJUU3JTkzJTlD&amp;ntb=1\">"
    "冬瓜【中药】【中医百科】</a></h2>"
    "<p>每服五十丸，煎冬瓜子汤送下，不拘时候。</p>"
    "</li>"
    // ③ 没有摘要 <p> 的条目
    "<li class=\"b_algo\">"
    "<h2><a href=\"https://zhuanlan.zhihu.com/p/136609826\">冬瓜最好吃的19种做法 - 知乎</a></h2>"
    "</li>"
    // ④ 非结果块：不能算进结果（b_ans 是"答案卡"，不是网页结果）
    "<li class=\"b_ans\"><h2><a href=\"https://example.com/answer\">答案卡片</a></h2></li>"
    "</ol></body></html>";

int main(int argc, char **argv)
{
    // 排障模式: test_search_parse <保存下来的必应页面.html>
    // 抓一份真实结果页存盘再跑, 用来判断"是解析器坏了还是页面变了"（文档 §6 的排查手段）
    if (argc > 1)
    {
        ifstream in(argv[1], ios::binary);
        if (!in)
        {
            printf("打不开文件: %s\n", argv[1]);
            return 2;
        }
        string html((istreambuf_iterator<char>(in)), istreambuf_iterator<char>());
        vector<string> real;
        bool parsed = jzParseBingHtmlResults(html, real);
        printf("%s: %d 字节, 解析出 %d 条\n", argv[1], (int)html.size(), (int)real.size());
        for (size_t i = 0; i < real.size() && i < 3; i++)
            printf("--- %d ---\n%s\n", (int)i + 1, real[i].c_str());
        return parsed ? 0 : 1;
    }

    vector<string> out;

    // ---------- ① 正常结果页 ----------
    bool ok = jzParseBingHtmlResults(kBingPage, out);
    check(ok, "能解析出结果（返回 true）");
    check(out.size() == 3, "3 条结果（b_ans 不算）: 实际 " + to_string(out.size()));

    if (out.size() == 3)
    {
        // 条目 1: <strong> 被清掉，链接是直链，摘要有 "·" 且无残留实体
        check(contains(out[0], "冬瓜（葫芦科冬瓜属植物）_百度百科"),
              "标题 1: 内联 <strong> 已清除");
        check(contains(out[0], "https://baike.baidu.com/item/%E5%86%AC%E7%93%9C/422112"),
              "链接 1: 直链原样保留");
        check(contains(out[0], "冬瓜（学名：Benincasa hispida）"),
              "摘要 1: 正文提取正确");
        check(contains(out[0], "·") && !contains(out[0], "&ensp;"),
              "摘要 1: &ensp;&#0183;&ensp; 还原为 · 且无残留实体");
        check(!contains(out[0], "<strong>") && !contains(out[0], "</p>"),
              "条目 1: 不含任何 HTML 标签");

        // 条目 2: ck/a 跳转链接本地解码
        check(contains(out[1], "https://zhongyibaike.com/wiki/%E5%86%AC%E7%93%9C"),
              "链接 2: ck/a 的 u=a1<base64url> 已解码为真实 URL");
        check(!contains(out[1], "/ck/a"),
              "链接 2: 不再含 bing 跳转包裹");

        // 条目 3: 无摘要，仍要有标题和链接
        check(contains(out[2], "冬瓜最好吃的19种做法 - 知乎"),
              "条目 3: 无摘要时标题仍在");
        check(contains(out[2], "https://zhuanlan.zhihu.com/p/136609826"),
              "条目 3: 无摘要时链接仍在");
    }

    // ---------- ② 验证页 / 无关页面 ----------
    {
        vector<string> none;
        string junk = "<html><body>请输入验证码 antispider</body></html>";
        check(!jzParseBingHtmlResults(junk, none) && none.empty(),
              "验证页/无关页面: 返回 false 且不产出条目");
    }

    // ---------- ③ 只有 <link> 标签（确保没把 <link> 当成 <li>） ----------
    {
        vector<string> none;
        string head = "<head><link rel=\"stylesheet\" href=\"/a.css\"></head><body></body>";
        check(!jzParseBingHtmlResults(head, none),
              "<link> 不被误当成 <li> 结果块");
    }

    // ---------- ④ 相关性判定（抓取型源降级保护） ----------
    // 源被限流/污染时返回 200 + 一堆无关条目，不判定就会被当答案喂给模型
    {
        vector<string> hit;
        hit.push_back("冬瓜（葫芦科冬瓜属植物）_百度百科\n  https://baike.baidu.com/x\n  冬瓜是…\n");
        check(jzSearchResultsRelevant("冬瓜", hit),
              "相关性: 结果含查询词 -> 判为相关");

        vector<string> junk;
        junk.push_back("Telegram Messenger\n  https://telegram.org/\n  Rich Text Editor…\n");
        check(!jzSearchResultsRelevant("冬瓜", junk),
              "相关性: 结果与查询零交集 -> 判为降级（报搜索不可用）");

        vector<string> multi;
        multi.push_back("Python asyncio 模块 - 菜鸟教程\n  https://x\n  async/await 用法\n");
        check(jzSearchResultsRelevant("python asyncio", multi),
              "相关性: 多词查询命中任一 token 即算相关");

        check(jzSearchResultsRelevant("a", junk),
              "相关性: 单字母查询无从判定 -> 放行（避免误杀）");
    }

    printf("\n%s (%d 个失败)\n", g_fail == 0 ? "ALL PASS" : "SOME FAILED", g_fail);
    return g_fail == 0 ? 0 : 1;
}
