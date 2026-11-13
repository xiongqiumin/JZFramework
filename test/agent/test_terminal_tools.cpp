// 终端工具类冒烟测试: terminal_run / terminal_cancel
//
// 为什么单独测这一层: 终端 API 本身已由 JZShell/JZVirtualTerminal 覆盖, 但**工具类**
// (JZTerminalTools.cpp) 的参数解析、回包文案、错误分支以前没有任何测试 —— 而那才是模型
// 真正看到的东西。
//
// 2026-11-12 起终端改成**池化 + 无状态执行**: 每个 agent 一个 JZTerminalPool, 每次
// terminal_run 借一台、跑完归还 —— 多条命令真并发, 各自独占一台终端; 到并发上限
// (maxLive)直接报忙(不排队)。没有 run_id / terminal_query / background 参数。
// 单台 JZVirtualTerminal 仍是"一次一条", 并发由池摊到多台; terminal_cancel 一次停全部。
// `cmd &` 起的后台进程不占前台槽, 也不归 cancel 管。
//
// 覆盖:
//   terminal_run: 前台跑完才回包 / 回包是原样输出 / 空 command /
//                 被拒命令**只回一次包** / `&` 立刻回包且不占前台槽 /
//                 两条并发各占一台 / 第三条撞并发上限被拒
//   terminal_cancel: 一次停掉所有在跑的 / 停完再停(没得停) / 全新池(没得停)
//   上下文阀: 超长输出只回末尾 + 一行标记
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
        m_pool.setConfig(cfg);
        m_pool.setMaxLive(2);        // 两条并发, 第三条正好撞上限
        m_pool.setKeepIdle(-1);      // 归还的终端全留着: 别在终端自己的线程里 delete 它
        m_freshPool.setConfig(cfg);  // 全新池: 从没跑过命令

        connect(&m_run.sigResult, [this](const ToolCallResult &r) { record(m_runRes, r); });
        connect(&m_cancel.sigResult, [this](const ToolCallResult &r) { record(m_cancelRes, r); });
        connect(&m_cancel2.sigResult, [this](const ToolCallResult &r) { record(m_cancel2Res, r); });

        printf("     workspace=%s\n\n", m_ws.c_str());

        // 兜底: 某个回调没来也不能把测试挂死
        JZTimer::singleShot(60000, [this]()
        {
            check(false, "超时: 60 秒还没跑完");
            jzApp->quit();
        });

        stepForeground();
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

    // 等异步回包凑够 target 条(或重试耗尽), 再走下一步: 命令什么时候退出不确定
    void waitCount(LastResult *r, int target, int retries, const std::function<void()> &next)
    {
        if (r->count >= target || retries <= 0)
        {
            next();
            return;
        }
        JZTimer::singleShot(100, [this, r, target, retries, next]()
        {
            waitCount(r, target, retries - 1, next);
        });
    }

    void waitResult(LastResult *r, int retries, const std::function<void()> &next)
    {
        waitCount(r, 1, retries, next);
    }

    // ---- 1. 前台: 跑完才回包, 回包就是命令的原样输出 ----
    void stepForeground()
    {
        m_runRes = LastResult();
        m_run.call(makeCall("c1", R"({"command":"echo fg_ok","timeout_ms":20000})"));
        check(m_runRes.count == 0, "前台跑: 命令没结束前不回包");

        waitResult(&m_runRes, 100, [this]() { stepForegroundCheck(); });
    }

    void stepForegroundCheck()
    {
        check(m_runRes.count == 1, "前台跑: 跑完只回一次包");
        check(m_runRes.ok, "前台跑: 成功: " + m_runRes.text);
        check(contains(m_runRes.text, "fg_ok"), "前台跑: 回包带输出");
        // 回包**不加工**: 就是命令的原样输出, 不带 [cwd] / [run_id] 那些尾巴
        check(!contains(m_runRes.text, "[cwd]") && !contains(m_runRes.text, "[run_id]"),
              "前台跑: 回包是原样输出(无 [cwd]/[run_id] 等附加内容): " + m_runRes.text);

        stepErrors();
    }

    // ---- 2. 错误分支: 一律"失败 + 只回一次包" ----
    void stepErrors()
    {
        m_runRes = LastResult();
        m_run.call(makeCall("e1", R"({"command":""})"));
        check(m_runRes.count == 1 && !m_runRes.ok, "空 command: 失败且只回一次包");
        check(contains(m_runRes.text, "command 不能为空"), "空 command: 文案正确: " + m_runRes.text);

        // 被拒的命令(审核阶段就挡掉): run() 同步报错, 工具不能再 finish 第二次
        m_runRes = LastResult();
        m_run.call(makeCall("e2", R"({"command":"rm -rf /"})"));
        check(m_runRes.count == 1, "被拒命令: 只回一次包, 实际 " + to_string(m_runRes.count));
        check(!m_runRes.ok, "被拒命令: 失败: " + m_runRes.text);

        // timeout_ms 太小会被静默改写 —— 回包必须把改写说出来
        m_runRes = LastResult();
        m_run.call(makeCall("e3", R"({"command":"echo note_ok","timeout_ms":500})"));
        waitResult(&m_runRes, 100, [this]() { stepErrorsCheck(); });
    }

    void stepErrorsCheck()
    {
        check(m_runRes.count == 1 && m_runRes.ok, "小 timeout: 成功且只回一次包");
        check(contains(m_runRes.text, "[提示]") && contains(m_runRes.text, "120000"),
              "小 timeout: 回包说明已按 120000 ms 处理: " + m_runRes.text);

        stepBackground();
    }

    // ---- 3. 后台: `cmd &` 立刻回包, 且不占池里的执行槽 ----
    void stepBackground()
    {
        m_runRes = LastResult();
        m_run.call(makeCall("b1", R"({"command":"sleep 2 &","timeout_ms":20000})"));
        // 收尾要回到终端线程, 不是同步的: 等一小会再看
        waitResult(&m_runRes, 50, [this]() { stepBackgroundCheck(); });
    }

    void stepBackgroundCheck()
    {
        check(m_runRes.count == 1, "后台 &: 只回一次包(没被 sleep 堵住)");
        check(m_runRes.ok, "后台 &: 成功: " + m_runRes.text);
        check(m_pool.cancelAll() == 0, "后台 &: 不占池里的执行槽(没有在跑的前台命令)");

        stepConcurrent();
    }

    // ---- 4. 并发: 两条各占一台终端; 第三条撞池上限 ----
    void stepConcurrent()
    {
        m_runRes = LastResult();
        m_run.call(makeCall("p1", R"({"command":"sleep 5","timeout_ms":20000})"));
        m_run.call(makeCall("p2", R"({"command":"sleep 5","timeout_ms":20000})"));
        check(m_runRes.count == 0, "并发: 两条长命令都还没回包(各占一台终端)");

        // 池里两台都借出去了: 第三条拿不到终端, 直接拒(不排队), 只回一次包
        m_run.call(makeCall("p3", R"({"command":"echo third"})"));
        check(m_runRes.count == 1 && !m_runRes.ok,
              "并发上限: 第三条被拒且只回一次包, 实际 " + to_string(m_runRes.count));
        check(contains(m_runRes.text, "并发上限 2 已满"),
              "并发上限: 文案点明原因: " + m_runRes.text);

        stepCancel();
    }

    // ---- 5. cancel: 一次停掉所有在跑的; 停完再停就没得停了 ----
    void stepCancel()
    {
        m_cancelRes = LastResult();
        m_cancel.call(makeCall("k1", R"({})"));
        check(m_cancelRes.count == 1 && m_cancelRes.ok, "cancel: 成功: " + m_cancelRes.text);
        check(contains(m_cancelRes.text, "已请求停止 2 条命令"),
              "cancel: 两条一起停: " + m_cancelRes.text);

        // 等 p1/p2 的取消回包(count 到 3: 含上面被拒的第三条), 再走下一步
        waitCount(&m_runRes, 3, 100, [this]() { stepCancelAgain(); });
    }

    void stepCancelAgain()
    {
        check(m_runRes.count == 3,
              "cancel: 两条都收到收尾回包(实际 " + to_string(m_runRes.count) + ")");
        check(contains(m_runRes.text, "命令已被取消"),
              "cancel: 回包说明是取消: " + m_runRes.text);

        m_cancelRes = LastResult();
        m_cancel.call(makeCall("k2", R"({})"));
        check(m_cancelRes.count == 1 && !m_cancelRes.ok, "再停一次: 失败(没有在跑的)");
        check(contains(m_cancelRes.text, "没有在跑的命令"), "再停一次: 文案正确: " + m_cancelRes.text);

        stepReplyCap();
    }

    // ---- 6. 上下文阀: 超长回包只回**末尾** + 一行标记说明怎么办 ----
    // 终端现在归池管, 外部拿不到单台终端去收紧阈值 —— 直接产出超默认 32KB 的输出来触发。
    void stepReplyCap()
    {
        m_runRes = LastResult();
        // 头尾各一个哨兵: 阀只留末尾 ⇒ 头部 head_marker 不该出现, 尾部 tail_marker 必须在
        m_run.call(makeCall("v1", R"({"command":"echo head_marker; seq 1 12000; echo tail_marker","timeout_ms":30000})"));
        waitResult(&m_runRes, 200, [this]() { stepReplyCapCheck(); });
    }

    void stepReplyCapCheck()
    {
        check(m_runRes.count == 1 && m_runRes.ok,
              "阀: 成功(回包 " + to_string(m_runRes.text.size()) + "B)");
        check(contains(m_runRes.text, "输出过长"),
              "阀: 触发时开头有显式标记(回包 " + to_string(m_runRes.text.size()) + "B)");
        check(contains(m_runRes.text, "tail -50"), "阀: 标记告诉模型自己过滤(tail)");
        check(contains(m_runRes.text, "read_file"), "阀: 标记给出重定向 + read_file 的路子");
        check(contains(m_runRes.text, "tail_marker"), "阀: 保留末尾(错误/结果在尾部)");
        check(!contains(m_runRes.text, "head_marker"), "阀: 头部被裁掉");
        check(m_runRes.text.size() < 40 * 1024,
              "阀: 回包压到 32KB 上限附近(实际 " + to_string(m_runRes.text.size()) + "B)");

        stepFreshPool();
    }

    // ---- 7. 全新池: 没有可停的东西 ----
    void stepFreshPool()
    {
        m_cancel2Res = LastResult();
        m_cancel2.call(makeCall("f2", R"({})"));
        check(m_cancel2Res.count == 1 && !m_cancel2Res.ok, "全新池 cancel: 失败且只回一次包");
        check(contains(m_cancel2Res.text, "没有在跑的命令"),
              "全新池 cancel: 文案正确: " + m_cancel2Res.text);

        jzApp->quit();
    }

    string m_ws;
    JZTerminalPool m_pool { 2 };        // 主池: 允许两条并发, 第三条撞上限
    JZTerminalPool m_freshPool { 1 };   // 全新池: 从没跑过命令
    JZTerminalRunTool m_run { &m_pool };
    JZTerminalCancelTool m_cancel { &m_pool };
    JZTerminalCancelTool m_cancel2 { &m_freshPool };

    LastResult m_runRes;
    LastResult m_cancelRes;
    LastResult m_cancel2Res;
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
