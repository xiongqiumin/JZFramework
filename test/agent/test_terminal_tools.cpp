// 终端三件套工具类的冒烟测试: terminal_run / terminal_query / terminal_cancel
//
// 为什么单独测这一层: 终端 API 本身已被 test_virtual_terminal 覆盖, 但**工具类**
// (JZTerminalTools.cpp) 的参数解析、回包文案、错误分支以前没有任何测试 —— 而那才是模型
// 真正看到的东西(尤其 background 那条: 启动即回 run_id, 之后靠 query/cancel 收尾)。
//
// 覆盖:
//   terminal_run: 后台起(background=true) 启动即回 [run_id] / 前台跑完才回 / 空 command /
//                 被拒命令**只回一次包**(防止重复 finish)
//   terminal_query: 按 run_id 查、省略 run_id 查最近一次、tail、grep(命中与不命中)、
//                   坏 run_id、全新终端(query 还没跑过)
//   terminal_cancel: 省略 run_id 停最近一条在跑的、停一条已结束的、全新终端(没得停)
// 期望输出: ALL PASS

#include <stdio.h>

#include <functional>
#include <string>

#include "JZApplication.h"
#include "JZDir.h"
#include "JZTerminalTools.h"
#include "JZTimer.h"
#include "JZVirtualTerminal.h"
#include "json/json.h"

using namespace std;

static int g_fail = 0;

static void check(bool ok, const string &what)
{
    printf("%s %s\n", ok ? "[ok]  " : "[fail]", what.c_str());
    if (!ok)
        g_fail++;
}

static bool contains(const string &text, const string &needle)
{
    return text.find(needle) != string::npos;
}

// 从回包文本里抠出 [run_id] xxx 那个 xxx(后台起的和前台跑完的格式一样)
static string extractRunId(const string &text)
{
    const string key = "[run_id] ";
    size_t p = text.find(key);
    if (p == string::npos)
        return string();
    p += key.size();
    size_t e = text.find_first_of(" \r\n", p);
    return text.substr(p, (e == string::npos) ? string::npos : e - p);
}

// 一次调用的回包: count 用来验"只回一次包"(工具被拒时 run() 会同步回一次, 调用方容易重复回)
struct LastResult
{
    int count = 0;
    bool ok = false;
    string text;
};

class TerminalToolTest
{
public:
    int run()
    {
        // 自己的临时工作区, 免得污染仓库
        m_ws = jzApp->applicationDirPath() + "/vt_tools_ws";
        JZDir(m_ws).removeRecursively();
        JZDir().mkpath(m_ws);

        JZVirtualTerminalConfig cfg;
        cfg.zone.workspace = m_ws;
        m_term.setId("t1");
        m_term.setTerminalConfig(cfg);
        m_term2.setId("t2");            // 全新终端: 用来测"还没有运行过任何命令"
        m_term2.setTerminalConfig(cfg);

        connect(&m_run.sigResult, [this](const ToolCallResult &r) { record(m_runRes, r); });
        connect(&m_query.sigResult, [this](const ToolCallResult &r) { record(m_queryRes, r); });
        connect(&m_cancel.sigResult, [this](const ToolCallResult &r) { record(m_cancelRes, r); });
        connect(&m_query2.sigResult, [this](const ToolCallResult &r) { record(m_query2Res, r); });
        connect(&m_cancel2.sigResult, [this](const ToolCallResult &r) { record(m_cancel2Res, r); });

        printf("     workspace=%s\n\n", m_ws.c_str());

        // 兜底: 某个回调没来也不能把测试挂死
        JZTimer::singleShot(60000, [this]()
        {
            check(false, "超时: 60 秒还没跑完");
            jzApp->quit();
        });

        stepRunBackground();
        return jzApp->exec();
    }

private:
    static void record(LastResult &out, const ToolCallResult &r)
    {
        out.count++;
        out.ok = r.isOk;
        out.text = r.result;
    }

    static ToolCall makeCall(const string &id, const string &argsJson)
    {
        ToolCall tc;
        tc.id = id;
        tc.type = "function";
        tc.function_args = argsJson;
        Json::Reader reader;
        if (!reader.parse(argsJson, tc.params))
            tc.params = Json::Value(Json::objectValue);
        return tc;
    }

    // 等异步回包(count>0)或重试耗尽, 再走下一步: 前台命令什么时候退出不确定
    void waitResult(LastResult *r, int retries, const std::function<void()> &next)
    {
        if (r->count > 0 || retries <= 0)
        {
            next();
            return;
        }
        JZTimer::singleShot(100, [this, r, retries, next]() { waitResult(r, retries - 1, next); });
    }

    // ---- 1. 后台起: 启动成功就回 run_id, 不等命令结束 ----
    void stepRunBackground()
    {
        m_runRes = LastResult();
        m_run.call(makeCall("c1",
                            R"({"command":"echo bg_start && sleep 30 && echo bg_end","background":true})"));

        check(m_runRes.count == 1, "后台起: 只回一次包");
        check(m_runRes.ok, "后台起: 回包成功: " + m_runRes.text);
        check(contains(m_runRes.text, "已后台启动"), "后台起: 文案点明是后台启动");
        check(contains(m_runRes.text, "terminal_query"), "后台起: 提示之后用 terminal_query 查");
        check(contains(m_runRes.text, "terminal_cancel"), "后台起: 提示用 terminal_cancel 停");

        m_ridBg = extractRunId(m_runRes.text);
        check(!m_ridBg.empty(), "后台起: 回包里带 [run_id]: " + m_runRes.text);
        check(m_term.info().runningCount == 1, "后台起: 命令还在跑(启动即返回, 没等它结束)");

        // 给它 1 秒把第一行输出刷出来, 再查(顺带验证"没有超时来杀它")
        JZTimer::singleShot(1000, [this]() { stepQuery(); });
    }

    // ---- 2. terminal_query: 五种查法 ----
    void stepQuery()
    {
        // 2.1 省略 run_id: 默认查最近一次运行
        m_queryRes = LastResult();
        m_query.call(makeCall("q1", R"({})"));
        check(m_queryRes.count == 1 && m_queryRes.ok, "query(省略 run_id): 成功: " + m_queryRes.text);
        check(contains(m_queryRes.text, m_ridBg), "query(省略 run_id): 查到的是刚起的那条");
        check(contains(m_queryRes.text, "[running]"), "query(省略 run_id): 状态 running");
        check(contains(m_queryRes.text, "已运行"), "query: 还在跑的带已运行时长");
        check(contains(m_queryRes.text, "[后台]"), "query: 标了 [后台]");
        check(contains(m_queryRes.text, "pid="), "query: 带 pid");
        check(contains(m_queryRes.text, "bg_start"), "query: 日志里有 bg_start");
        check(contains(m_queryRes.text, "[command] echo bg_start"), "query: 回显命令原文");

        // 2.2 显式 run_id + tail + from_last=false(= 看历史: 回最后 N 行, 不走增量)
        m_queryRes = LastResult();
        m_query.call(makeCall("q2", "{\"run_id\":\"" + m_ridBg + "\",\"tail\":1,\"from_last\":false}"));
        check(m_queryRes.count == 1 && m_queryRes.ok, "query(run_id+tail=1): 成功: " + m_queryRes.text);
        check(contains(m_queryRes.text, "[日志 1 行"), "query(tail=1): 只回最后 1 行: " + m_queryRes.text);
        check(contains(m_queryRes.text, "(第 1..1 行"), "query(tail=1): 回包带行号区间");

        // 2.3 grep 命中
        m_queryRes = LastResult();
        m_query.call(makeCall("q3", "{\"run_id\":\"" + m_ridBg + "\",\"grep\":\"bg_start\"}"));
        check(m_queryRes.count == 1 && m_queryRes.ok, "query(grep 命中): 成功: " + m_queryRes.text);
        check(contains(m_queryRes.text, "grep=bg_start"), "query(grep): 回包标出过滤条件");
        check(contains(m_queryRes.text, "bg_start"), "query(grep 命中): 该行还在");

        // 2.4 grep 不命中: 过滤后 0 行, 但**不是失败**(要跟"坏 run_id"区分开)
        m_queryRes = LastResult();
        m_query.call(makeCall("q4", "{\"run_id\":\"" + m_ridBg + "\",\"grep\":\"jz_no_such_line\"}"));
        check(m_queryRes.count == 1 && m_queryRes.ok, "query(grep 无命中): 仍然成功");
        check(contains(m_queryRes.text, "[日志 0 行"), "query(grep 无命中): 0 行: " + m_queryRes.text);
        check(contains(m_queryRes.text, "没有符合过滤条件的行"), "query(grep 无命中): 明说是过滤没命中");
        // ⚠ 只看日志行: 头部的 [command] 原样回显命令, 里面当然有 bg_start
        check(!contains(m_queryRes.text, "stdout| bg_start"), "query(grep 无命中): 命中的行确实被滤掉了");

        // 2.5 坏 run_id: 失败 + 顺手给出最近可用的 id
        m_queryRes = LastResult();
        m_query.call(makeCall("q5", R"({"run_id":"t1-r9999"})"));
        check(m_queryRes.count == 1 && !m_queryRes.ok, "query(坏 run_id): 失败: " + m_queryRes.text);
        check(contains(m_queryRes.text, "没有这个 run_id"), "query(坏 run_id): 文案点明原因");
        check(contains(m_queryRes.text, m_ridBg), "query(坏 run_id): 提示最近的 run_id");

        stepCancel();
    }

    // ---- 3. terminal_cancel: 省略 run_id 停最近一条在跑的 ----
    void stepCancel()
    {
        m_cancelRes = LastResult();
        m_cancel.call(makeCall("k1", R"({})"));
        check(m_cancelRes.count == 1 && m_cancelRes.ok, "cancel(省略 run_id): 成功: " + m_cancelRes.text);
        check(contains(m_cancelRes.text, m_ridBg), "cancel(省略 run_id): 停的正是那条在跑的");
        check(contains(m_cancelRes.text, "terminal_query"), "cancel: 提示收尾结果用 query 查");
        check(m_term.info().runningCount == 0, "cancel 之后一条都不剩");

        // 停完立刻查: kill 是同步收尾, 状态应当已经是 done
        m_queryRes = LastResult();
        m_query.call(makeCall("q6", "{\"run_id\":\"" + m_ridBg + "\"}"));
        check(m_queryRes.count == 1 && m_queryRes.ok, "取消后再查: 成功: " + m_queryRes.text);
        check(contains(m_queryRes.text, "[done]"), "取消后再查: 状态转 done: " + m_queryRes.text);
        check(contains(m_queryRes.text, "exit="), "取消后再查: 带退出码");
        check(!contains(m_queryRes.text, "[running]"), "取消后再查: 不再是 running");
        // 这条前面已经查过(游标在动) ⇒ 现在按增量: 没有新输出就别把老日志再贴一遍
        check(contains(m_queryRes.text, "没有新增输出"), "取消后再查: 只报没有新增(不重复贴日志)");

        // 再停同一条: 已经不在跑了
        m_cancelRes = LastResult();
        m_cancel.call(makeCall("k2", "{\"run_id\":\"" + m_ridBg + "\"}"));
        check(m_cancelRes.count == 1 && !m_cancelRes.ok, "再停同一条: 失败(已结束)");
        check(contains(m_cancelRes.text, "已经不在跑了"), "再停同一条: 文案点明已结束: " + m_cancelRes.text);

        stepQueryIncremental();
    }

    // ---- 3.5 增量查询: 默认只回"上次之后的新增行"(轮询查进度不重复灌上下文) ----
    void stepQueryIncremental()
    {
        m_runRes = LastResult();
        m_run.call(makeCall("c3", R"({"command":"echo inc_1 && sleep 3 && echo inc_2 && sleep 30","background":true})"));
        check(m_runRes.ok, "增量: 后台起一条会陆续输出的命令: " + m_runRes.text);
        m_incRunId = extractRunId(m_runRes.text);
        check(!m_incRunId.empty(), "增量: 拿到 run_id: " + m_runRes.text);

        // 给它一点时间把第一行刷出来
        JZTimer::singleShot(800, [this]() { stepIncFirstQuery(); });
    }

    void stepIncFirstQuery()
    {
        check(m_term.info().runningCount >= 1, "增量: 那条后台命令还在跑");

        // 第一次查这条运行(readPos=0) => 看最近一段, 不是增量
        m_queryRes = LastResult();
        m_query.call(makeCall("q8", "{\"run_id\":\"" + m_incRunId + "\"}"));
        check(m_queryRes.ok && contains(m_queryRes.text, "inc_1"),
              "增量: 第一次查能看到已有输出: " + m_queryRes.text);
        check(!contains(m_queryRes.text, "只看上次之后的新增"), "增量: 第一次查不走增量");

        // 等第二行出来再查
        JZTimer::singleShot(3200, [this]() { stepQueryIncrementalCheck(); });
    }

    void stepQueryIncrementalCheck()
    {
        m_queryRes = LastResult();
        m_query.call(makeCall("q9", "{\"run_id\":\"" + m_incRunId + "\"}"));
        check(m_queryRes.ok, "增量: 第二次查成功: " + m_queryRes.text);
        check(contains(m_queryRes.text, "只看上次之后的新增"), "增量: 第二次查转成增量模式");
        check(contains(m_queryRes.text, "inc_2"), "增量: 回的是新出现的那行(inc_2)");
        check(!contains(m_queryRes.text, "stdout| inc_1"), "增量: 已经看过的那行不再重复回");

        // 没有新输出时: 明说"没有新增", 而不是把老日志再贴一遍
        m_queryRes = LastResult();
        m_query.call(makeCall("q10", "{\"run_id\":\"" + m_incRunId + "\"}"));
        check(m_queryRes.ok && contains(m_queryRes.text, "暂无新增输出"), "增量: 无新增时明确说明");
        check(!contains(m_queryRes.text, "stdout| inc_2"), "增量: 无新增时不重复贴旧日志");

        // from_last=false + tail=0: 看历史(完整日志都回来, 且不动增量游标)
        m_queryRes = LastResult();
        m_query.call(makeCall("q11",
                              "{\"run_id\":\"" + m_incRunId + "\",\"from_last\":false,\"tail\":0}"));
        check(m_queryRes.ok, "看历史(from_last=false): 成功: " + m_queryRes.text);
        check(contains(m_queryRes.text, "stdout| inc_1") && contains(m_queryRes.text, "stdout| inc_2"),
              "看历史: 完整日志都在(inc_1 + inc_2)");

        // 看历史 + tail 限制: 要说明更早的行没显示
        m_queryRes = LastResult();
        m_query.call(makeCall("q12",
                              "{\"run_id\":\"" + m_incRunId + "\",\"from_last\":false,\"tail\":1}"));
        check(m_queryRes.ok && contains(m_queryRes.text, "更早的 1 行未显示"),
              "看历史(tail=1): 说明更早的行未显示: " + m_queryRes.text);
        check(!contains(m_queryRes.text, "stdout| inc_1"), "看历史(tail=1): 只回最后 1 行");

        m_cancelRes = LastResult();
        m_cancel.call(makeCall("k3", "{\"run_id\":\"" + m_incRunId + "\"}"));
        check(m_cancelRes.ok, "增量: 收尾停掉那条后台命令");

        stepRunForeground();
    }

    // ---- 4. 前台: 跑完才回包, 之后照样能用 query 取完整日志 ----
    void stepRunForeground()
    {
        m_runRes = LastResult();
        m_run.call(makeCall("c2", R"({"command":"echo fg_ok","timeout_ms":20000})"));
        check(m_runRes.count == 0, "前台跑: 命令没结束前不回包");

        waitResult(&m_runRes, 100, [this]() { stepRunForegroundCheck(); });
    }

    void stepRunForegroundCheck()
    {
        check(m_runRes.count == 1, "前台跑: 跑完只回一次包");
        check(m_runRes.ok, "前台跑: 成功: " + m_runRes.text);
        check(contains(m_runRes.text, "fg_ok"), "前台跑: 回包带输出");
        // 2026-10-14: 回包**不再加工** —— 就是命令的原样输出, 不带 [cwd] / [run_id] 那些尾巴
        check(!contains(m_runRes.text, "[cwd]") && !contains(m_runRes.text, "[run_id]"),
              "前台跑: 回包是原样输出(无 [cwd]/[run_id] 等附加内容): " + m_runRes.text);

        // 前台不再回 run_id(要查日志就用「省略 run_id = 查最近一次运行」)
        m_queryRes = LastResult();
        m_query.call(makeCall("q7", R"({})"));
        check(m_queryRes.count == 1 && m_queryRes.ok,
              "query(省略 run_id = 最近一次, 即前台刚跑完那条): 成功: " + m_queryRes.text);
        check(contains(m_queryRes.text, "fg_ok"), "query: 取到完整日志");
        check(contains(m_queryRes.text, "[done]"), "query: 状态 done");

        stepErrors();
    }

    // ---- 5. 错误分支: 一律"失败 + 只回一次包" ----
    void stepErrors()
    {
        m_runRes = LastResult();
        m_run.call(makeCall("e1", R"({"command":""})"));
        check(m_runRes.count == 1 && !m_runRes.ok, "空 command: 失败且只回一次包");
        check(contains(m_runRes.text, "command 不能为空"), "空 command: 文案正确: " + m_runRes.text);

        // 被拒的命令: run() 会**同步**回调一次 ok=false, 工具不能再 finish 第二次
        m_runRes = LastResult();
        m_run.call(makeCall("e2", R"({"command":"cd deep"})"));
        check(m_runRes.count == 1, "被拒命令(前台): 只回一次包, 实际 " + to_string(m_runRes.count));
        check(!m_runRes.ok, "被拒命令(前台): 失败: " + m_runRes.text);

        m_runRes = LastResult();
        m_run.call(makeCall("e3", R"({"command":"cd deep","background":true})"));
        check(m_runRes.count == 1 && !m_runRes.ok,
              "被拒命令(后台): 只回一次包且失败, 实际 " + to_string(m_runRes.count) + " 次");

        stepFreshTerminal();
    }

    // ---- 6. 全新终端: 没有可查/可停的东西 ----
    void stepFreshTerminal()
    {
        m_query2Res = LastResult();
        m_query2.call(makeCall("f1", R"({})"));
        check(m_query2Res.count == 1 && !m_query2Res.ok, "全新终端 query: 失败且只回一次包");
        check(contains(m_query2Res.text, "还没有运行过任何命令"),
              "全新终端 query: 文案正确: " + m_query2Res.text);

        m_cancel2Res = LastResult();
        m_cancel2.call(makeCall("f2", R"({})"));
        check(m_cancel2Res.count == 1 && !m_cancel2Res.ok, "全新终端 cancel: 失败且只回一次包");
        check(contains(m_cancel2Res.text, "没有在跑的命令"),
              "全新终端 cancel: 文案正确: " + m_cancel2Res.text);

        jzApp->quit();
    }

    string m_ws;
    JZVirtualTerminal m_term;
    JZVirtualTerminal m_term2;
    JZTerminalRunTool m_run { &m_term };
    JZTerminalQueryTool m_query { &m_term };
    JZTerminalCancelTool m_cancel { &m_term };
    JZTerminalQueryTool m_query2 { &m_term2 };
    JZTerminalCancelTool m_cancel2 { &m_term2 };

    LastResult m_runRes;
    LastResult m_queryRes;
    LastResult m_cancelRes;
    LastResult m_query2Res;
    LastResult m_cancel2Res;
    string m_ridBg;
    string m_incRunId;   // 增量查询那一段用的 run_id
};

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    // 输出重定向到管道时 stdout 是全缓冲的, 中途崩了会一个字都看不到
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("[step] main\n");

    JZApplication app;
    printf("[step] app ready, dir=%s\n", app.applicationDirPath().c_str());

    TerminalToolTest test;
    printf("[step] terminal tool test ready\n");

    int ret = test.run();

    printf("\n%s (fail=%d)\n", g_fail == 0 ? "ALL PASS" : "FAILED", g_fail);
    return g_fail == 0 ? ret : 1;
}
