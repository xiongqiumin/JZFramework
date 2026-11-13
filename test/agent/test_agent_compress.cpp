// 上下文压缩(TokenCompress)长跑观测: 盯 JZAgent::sigLLMHistory 抓"压缩发生"
//
// 为什么要单独测这个:
//   1) 压缩以前完全不可观测 —— 没有日志、没有独立事件, 只能靠"上下文突然变小"猜。
//      唯一确定的信号点是 onCompressFinished() 里 hist.clear() + push 摘要那一次
//      sigLLMHistory.emit(摘要消息, kind=Summary) ⇒ **收到 Summary 就是压缩发生了**。
//   2) 触发条件只有一条: llmLoop() 入口的 windowTokens() > maxToken。而
//      windowTokens() = 最近一条带 usage 的历史的 prompt_tokens(= 那一轮真正发出去的整段
//      prompt ⇒ 上一次请求时的上下文快照; 之后新增的消息/待发消息都不计 ⇒ 是当前上下文的下界)。
//
// 任务 = 让模型"分析代码"(每轮评审**同一份固定的 ~1000 行源码**):
//   ⭐ 为什么从"写 150 词"改成"分析代码": 写字的增长只有 2~4k/轮, 到 150k 要几十轮、好几小时;
//      换成整份源码(约 37KB ≈ 12k token)每轮塞进 user 消息 ⇒ 每轮涨 ≈12k+回答, 十几轮就压。
//      源码每轮都要**重发一遍**(历史里每轮各留一份), 这正是真 agent 反复读文件/贴代码的形态。
//   ⭐ 文件固定(kCodeFilePath; 想换就传 argv[1]), 内容是随便挑的一份真实实现文件 ——
//      只为让上下文涨得够快, 不关心分析结论对不对。
//
// 跑法(通常不带参数): 就用当前生产设置 —— 直连默认端点, maxToken = 150000,
//       **一直跑**, 压缩出现就打印(并断言"触发时 window > maxToken"), 然后继续下一轮, 不退出。
//       ⚠ 真实模型长跑: 一轮可能几分钟(思考 token 很多) ⇒ 挂着跑。
//       停: Ctrl+C, 或 Stop-Process -Name test_agent_compress。
//
// 每次压缩时的断言:
//   ① 触发时 window > maxToken(压缩只会晚, 不可能提前)
//   ② 压缩 = 整段历史被换成一条 Summary(不是"替换最旧一段"), 压缩后 window 归 0
//   ③ 压缩后本轮照常跑完(摘要 + 本轮 user -> 正常回答)
//
// 输出: 统一走 jzDebug()(JZLogManager) —— Console 侧会做 utf8→本地编码转换, 中文在
//       控制台不再花屏; 重定向到文件时是**本地编码(GBK)**, 读日志按 936 解。
//      每轮打印上下文两行, 用来跟模型面板对照:
//        [发请求前] 当前上下文 windowTokens()=X / 150000 (y%) + 最近一轮实测 prompt/completion 拆分
//        [本轮实测] prompt=P(= 本次请求发出去的上下文) completion=C => 当前上下文 = P+C (= 面板上的"上下文")

#include <stdio.h>

#include <chrono>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "JZAgent.h"
#include "JZAgentTool.h"
#include "JZApplication.h"
#include "JZDebug.h"
#include "JZTimer.h"

using namespace std;

// ---- 当前设置(要改就改这里) ----
// ⚠ 端点/模型名不入库(2026-11-09): 跑之前在这里填上本机端点, 或改回成带值再编译
static const char *kModelUrl = "";
static const char *kModelName = "";
static const int kMaxToken = 150000;     // 上下文预算
static const bool kThinkEnable = true;   // 与生产一致: 开着思考
static const int kMaxErrorStreak = 5;    // 连续出错这么多轮才收手(免得空转刷屏)

// ⭐ 固定要分析的源码文件(约 1000 行 / ≈37KB ≈ 12k token) —— 换文件改这里
static const char *kCodeFilePath = "C:/Users/7052/Desktop/other/JZFramework/JZNet/JZNetObject.cpp";
// kCodeFilePath 找不到时按顺序试的相对路径(从仓库根跑 / 从 build/test/agent/Release 跑都能命中)
static const char *kCodePathFallbacks[] = {
    "JZNet/JZNetObject.cpp",
    "../../../../JZNet/JZNetObject.cpp",
};

// 每轮换一个评审角度 —— 免得模型每轮写一模一样的话(那就成了复读, 上下文照涨但没意义)
static const char *kReviewFocuses[] = {
    "内存与资源管理(泄漏、重复释放、提前返回路径上的清理)",
    "并发与线程安全(跨线程回调、共享状态、时序假设)",
    "错误处理(返回值/错误码检查、失败分支、超时)",
    "接口设计(命名、参数、默认值、对调用方的契约)",
    "边界条件(空指针、越界、长度为 0、超大报文)",
    "性能与不必要的拷贝/分配",
};

static int g_fail = 0;
static int g_compressCount = 0;

static void check(bool ok, const string &what)
{
    jzDebug().nospace() << (ok ? "[ok]  " : "[fail]") << " " << what.c_str();
    if (!ok)
        g_fail++;
}

// 一行日志(整行原样输出, 不加引号/不加空格)
static void logLine(const string &text)
{
    jzDebug().nospace() << text.c_str();
}

static string fmtSec(double sec)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1fs", sec);
    return buf;
}

// 占预算百分比(150000*100 不会溢出 int)
static int pct(int value)
{
    return kMaxToken > 0 ? value * 100 / kMaxToken : 0;
}

// 整份读进来(二进制读; 源码是 UTF-8, 原样当字节流发给模型, 不转码)
static bool readWholeFile(const string &path, string &out, string &why)
{
    ifstream in(path.c_str(), ios::binary);
    if (!in.is_open())
    {
        why = "打不开";
        return false;
    }
    stringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    if (out.empty())
    {
        why = "是空文件";
        return false;
    }
    return true;
}

static int countLines(const string &text)
{
    int n = 0;
    for (size_t i = 0; i < text.size(); i++)
        if (text[i] == '\n')
            n++;
    if (!text.empty() && text[text.size() - 1] != '\n')
        n++;    // 末行没有换行也算一行
    return n;
}

// 定位要分析的文件: argv[1] > kCodeFilePath > 相对路径候选
static bool loadCodeFile(const string &overridePath, string &code, string &path, string &why)
{
    vector<string> candidates;
    if (!overridePath.empty())
        candidates.push_back(overridePath);
    candidates.push_back(kCodeFilePath);
    for (size_t i = 0; i < sizeof(kCodePathFallbacks) / sizeof(kCodePathFallbacks[0]); i++)
        candidates.push_back(kCodePathFallbacks[i]);

    why.clear();
    for (size_t i = 0; i < candidates.size(); i++)
    {
        string err;
        if (readWholeFile(candidates[i], code, err))
        {
            path = candidates[i];
            return true;
        }
        why += "[" + candidates[i] + " " + err + "] ";
    }
    return false;
}

// 工具基类: call() 里同步回结果(与 test_agent.cpp 的 FunctionTool 同形)
class FunctionTool : public JZAgentTool
{
public:
    std::function<ToolCallResult(const ToolCall &)> function;
    void call(const ToolCall &params) override
    {
        sigResult.emit(function(params));
    }
};

// ---------------------------------------------------------------------------
// 测试主体: 不退出, 一直跑
// ---------------------------------------------------------------------------
class CompressTest : public JZObject
{
public:
    int run(const std::string &fileOverride)
    {
        m_start = std::chrono::steady_clock::now();

        // 先把要评审的源码读进来(读不到直接报错退出, 免得空跑)
        std::string why;
        if (!loadCodeFile(fileOverride, m_code, m_codePath, why))
        {
            jzError().nospace() << "读不到要分析的源码文件: " << why.c_str();
            return 1;
        }
        m_codeLines = countLines(m_code);

        if (!*kModelUrl || !*kModelName)
        {
            jzError().nospace() << "未配置模型端点: 编辑本文件顶部的 kModelUrl/kModelName(端点/模型名不入库)";
            return 1;
        }

        JZAgentConfig cfg;
        cfg.modelUrl = kModelUrl;
        cfg.modelName = kModelName;
        cfg.maxRounds = 1000;          // 单轮内的工具循环上限(与压缩无关, 给大点)
        cfg.maxToken = kMaxToken;
        cfg.tokenStrategy = JZAgentConfig::TokenCompress;   // agent_cli / code_agent 都是这个策略
        cfg.netTimeout = 20 * 60 * 1000;   // 整份源码 + 思考 ⇒ 比"写 150 词"慢, 给足 10 分钟
        cfg.reuseConnection = false;   // 与 agent_cli 一致: 用完即关, 避开 keep-alive 时序

        m_agent = new JZAgent(this);
        m_agent->setStreamEnabled(false);       // 走非流式整包(与压缩请求同一条解析路径)
        // 旧的 setThinkEnabled(bool) 已换成三态 ThinkMode: On/Off 都会显式发
        // enable_thinking, Auto 才是不发字段(最兼容)。这里保持旧语义 = 显式发。
        m_agent->setThinkMode(kThinkEnable ? ThinkMode::On : ThinkMode::Off);
        m_agent->init(cfg);
        m_agent->setSystemPrompt("You are a code reviewer. Answer in plain text (no code blocks).");
        m_agent->addTool(makeReadFileTool(m_code));   // 真实 agent 都带工具: 复现压缩请求的真实形状

        connect(&m_agent->sigLLMHistory, [this](std::shared_ptr<LLMHistory> h) { onHistory(h); });
        connect(&m_agent->sigFinished, [this]() { onFinished(); });
        connect(&m_agent->sigError, [this](std::string msg) { onError(msg); });

        jzDebug().nospace() << "     设置: maxToken=" << kMaxToken
                            << "  model=" << kModelName
                            << "  think=" << (kThinkEnable ? "on" : "off");
        jzDebug().nospace() << "     url=" << kModelUrl;
        jzDebug().nospace() << "     分析文件: " << m_codePath.c_str() << " (" << m_codeLines << " 行, "
                            << (int)m_code.size() << " 字节 ≈ " << (int)(m_code.size() / 3)
                            << " token/轮 => 每轮上下文涨这么多)";
        jzDebug().nospace() << "     window = 最近一轮实测的 prompt(= 上次发出去的上下文);  "
                               "压缩 = sigLLMHistory 里出现 kind=Summary(一直跑, 不停)";

        startRound(1);
        return jzApp->exec();
    }

private:
    double elapsedSec() const
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - m_start).count();
    }

    // 工具: read_file —— 固定只服务这一份被测文件(测试不需要真文件系统工具)
    static JZAgentTool *makeReadFileTool(const std::string &code)
    {
        ToolDefine def;
        def.name = "read_file";
        def.description = "read the code file under review (test only; always returns the same file)";
        ParamSchema p;
        p.name = "path";
        p.type = ParamType::String;
        p.description = "file path (any value returns the fixed test file)";
        def.parameters.push_back(p);
        def.required_parameters.push_back("path");   // 空 arguments 不兜底 ⇒ 必须声明必填

        auto *tool = new FunctionTool();
        tool->setToolDefine(def);
        tool->function = [code](const ToolCall &call) -> ToolCallResult {
            ToolCallResult r;
            r.id = call.id;
            r.isOk = true;
            r.result = code;
            return r;
        };
        return tool;
    }

    // 发起第 n 轮: 记下"进入 llmLoop 时会用到的 window"(= 压缩判定用的那个数)
    void startRound(int n)
    {
        m_round = n;
        m_windowAtRoundStart = m_agent->windowTokens();
        m_histAtRoundStart = (int)m_agent->context()->history.size();
        m_summaryCountAtRoundStart = g_compressCount;
        const string prompt = promptOf(n);      // 整份源码只在发出去那一刻拼一次
        logLine("");
        jzDebug().nospace() << "---- 第 " << n << " 轮 ----";
        logContextSnapshot("发请求前");
        jzDebug().nospace() << "     [" << fmtSec(elapsedSec()).c_str() << "] 发: " << clip(prompt).c_str()
                            << " (prompt " << (int)prompt.size() << " 字节)";
        m_agent->request(prompt);
    }

    // ⭐ 上下文快照: 判定值 + 最近一次服务端实测的拆分 —— 方便跟模型面板上的"上下文"逐轮对照
    void logContextSnapshot(const char *tag)
    {
        const auto &hist = m_agent->context()->history;
        const int win = m_agent->windowTokens();
        jzDebug().nospace() << "     [" << tag << "] 当前上下文 windowTokens()=" << win << " / "
                            << kMaxToken << " (" << pct(win) << "%)   历史=" << hist.size() << " 条";
        for (size_t i = hist.size(); i > 0; --i)
        {
            const auto &h = hist[i - 1];
            if (h && h->usage.prompt_tokens + h->usage.completion_tokens > 0)
            {
                jzDebug().nospace() << "              最近一轮实测: prompt=" << h->usage.prompt_tokens
                                    << "(= 那次请求发出去的上下文) completion="
                                    << h->usage.completion_tokens
                                    << " cached=" << h->usage.cached_tokens
                                    << "  =>  prompt+completion="
                                    << (h->usage.prompt_tokens + h->usage.completion_tokens)
                                    << " (= 面板上那条请求的\"上下文\")";
                return;
            }
        }
        jzDebug().nospace() << "              还没有实测值(本轮是第一条请求)";
    }

    // 每轮的任务 = 换一个角度评审同一份源码; 整份源码贴进 user 消息(每轮各一份 ⇒ 上下文快速上涨)
    string promptOf(int n) const
    {
        const int focusCount = (int)(sizeof(kReviewFocuses) / sizeof(kReviewFocuses[0]));
        const char *focus = kReviewFocuses[(n - 1) % focusCount];

        string prompt = "第 " + to_string(n) + " 轮代码评审。下面是文件 `" + m_codePath + "` 的完整源码(共 " +
                        to_string(m_codeLines) + " 行)。\n请只从「" + focus +
                        "」这个角度, 逐条指出这份代码里**具体**的问题(点名函数/字段, 说明为什么算问题), "
                        "中文约 200 字; 不要复述源码, 不要输出代码块。\n\n---- 源码开始 ----\n" +
                        m_code + "\n---- 源码结束 ----\n";
        return prompt;
    }

    // 每入一条历史都会来(用户 / 助手 / 工具返回 / **压缩摘要**)
    void onHistory(std::shared_ptr<LLMHistory> h)
    {
        if (!h)
            return;

        const bool isSummary = (h->kind == LLMHistory::Summary);
        if (isSummary)
        {
            g_compressCount++;
            m_compressWindow = m_windowAtRoundStart;      // 触发本轮压缩的 window
            m_compressHistBefore = m_histAtRoundStart;    // 压缩前的历史条数
            logLine("");
            jzDebug().nospace() << "######## 压缩 #" << g_compressCount << " 发生: 第 " << m_round
                                << " 轮入口 window=" << m_windowAtRoundStart
                                << " > maxToken=" << kMaxToken
                                << ", 压缩前历史 " << m_histAtRoundStart
                                << " 条, t=" << fmtSec(elapsedSec()).c_str() << " ########";
            check(m_windowAtRoundStart > kMaxToken,
                  "压缩 #" + to_string(g_compressCount) + ": 触发时 window=" +
                      to_string(m_windowAtRoundStart) + " > maxToken=" + to_string(kMaxToken));
            // 现状: 摘要消息没有 usage ⇒ 压缩后判定基准被重置为 0
            check(m_agent->windowTokens() == 0,
                  "压缩后 window 归 0(摘要无 usage) => onCompressFinished() 里那次复查永远不会再触发压缩");
            check((int)m_agent->context()->history.size() == 1,
                  "压缩 = 整段历史被清空成 1 条摘要(压缩前 " + to_string(m_histAtRoundStart) + " 条)");
        }

        const string tag = isSummary ? "**压缩摘要**" : roleName(h->role);
        jzDebug().nospace() << "  [" << (m_timelineSize + 1) << "] " << tag.c_str()
                            << " kind=" << kindName(h->kind).c_str()
                            << " content=" << clip(h->content).c_str()
                            << " | window=" << m_agent->windowTokens()
                            << " 历史=" << m_agent->context()->history.size() << " 条";
        m_timelineSize++;
    }

    void onFinished()
    {
        const LLMContext *ctx = m_agent->context();
        jzDebug().nospace() << "     [" << fmtSec(elapsedSec()).c_str() << "] 本轮结束";
        reportUsage();
        m_errorStreak = 0;

        // 这一轮里发生过压缩 ⇒ 校验"压缩后本轮照常跑完"
        if (g_compressCount > m_summaryCountAtRoundStart)
        {
            check(ctx->history.size() >= 3,
                  "压缩后本轮跑完: 历史 = 摘要 + user + assistant(实际 " +
                      to_string(ctx->history.size()) + " 条)");
            check(!ctx->history.empty() && ctx->history.back()->role == LLMHistory::Assistant
                      && !ctx->history.back()->content.empty(),
                  "压缩后本轮拿到了 assistant 回答(压缩没有打断本轮对话)");
            check(!ctx->history.empty() && ctx->history[0] && ctx->history[0]->kind == LLMHistory::Summary,
                  "history[0] 是那条压缩摘要(kind=Summary)");
        }

        jzDebug().nospace() << "     == 累计: 轮数=" << m_round
                            << " 压缩=" << g_compressCount
                            << " fail=" << g_fail
                            << " 运行=" << fmtSec(elapsedSec()).c_str() << " ==";

        startRound(m_round + 1);   // 一直跑
    }

    // 本轮服务端实测(直接可跟模型面板对照)
    void reportUsage()
    {
        const auto &hist = m_agent->context()->history;
        for (size_t i = hist.size(); i > 0; --i)
        {
            const auto &h = hist[i - 1];
            if (h && h->usage.prompt_tokens + h->usage.completion_tokens > 0)
            {
                const int p = h->usage.prompt_tokens;
                const int c = h->usage.completion_tokens;
                jzDebug().nospace() << "       [本轮实测] prompt=" << p
                                    << "(= 本次请求发出去的上下文) completion=" << c
                                    << " cached=" << h->usage.cached_tokens
                                    << "  => 当前上下文 = " << (p + c) << " / " << kMaxToken
                                    << " (" << pct(p + c) << "%)";
                return;
            }
        }
        logLine("       [本轮实测] 没有 usage(锚点未更新, 下一轮判定值还是上一轮的)");
    }

    // 出错(连接失败/超时/压缩返空...)不退出: 记一次, 歇 3 秒开下一轮
    void onError(std::string msg)
    {
        m_errorStreak++;
        jzError().nospace() << "     [error] 第 " << m_round << " 轮失败(连续 " << m_errorStreak
                            << "/" << kMaxErrorStreak << "): " << msg.c_str();
        if (m_errorStreak >= kMaxErrorStreak)
        {
            jzError().nospace() << "     连续 " << m_errorStreak << " 轮出错, 收工(fail=" << g_fail << ")";
            jzApp->quit();
            return;
        }
        JZTimer::singleShot(3000, [this]() { startRound(m_round + 1); });   // 歇一下再来
    }

    static const char *roleName(LLMHistory::Role r)
    {
        switch (r)
        {
        case LLMHistory::User:      return "user";
        case LLMHistory::Assistant: return "assistant";
        case LLMHistory::System:    return "system";
        case LLMHistory::Tool:      return "tool";
        }
        return "?";
    }

    static string kindName(LLMHistory::Kind k)
    {
        switch (k)
        {
        case LLMHistory::Message:      return "Message";
        case LLMHistory::ContextStart: return "ContextStart";
        case LLMHistory::Summary:      return "Summary";
        }
        return "?";
    }

    static string clip(const string &text)
    {
        string s = text;
        for (size_t i = 0; i < s.size(); i++)
            if (s[i] == '\n' || s[i] == '\r')
                s[i] = ' ';
        if (s.size() > 40)
            s = s.substr(0, 40) + "...";
        return "\"" + s + "\"";
    }

    JZAgent *m_agent = nullptr;

    string m_code;                      // 被评审的源码(整份)
    string m_codePath;                  // 实际读到的路径
    int m_codeLines = 0;                // 源码行数

    int m_round = 0;                    // 当前第几轮
    int m_windowAtRoundStart = 0;       // 本轮入口处的 window(压缩判定用的数)
    int m_histAtRoundStart = 0;         // 本轮入口处的历史条数
    int m_summaryCountAtRoundStart = 0; // 本轮入口时的压缩次数
    int m_compressWindow = 0;           // 最近一次压缩触发时的 window
    int m_compressHistBefore = 0;       // 最近一次压缩前的历史条数
    int m_timelineSize = 0;             // 已打印的历史条数
    int m_errorStreak = 0;              // 连续出错轮数
    std::chrono::steady_clock::time_point m_start;
};

int main(int argc, char *argv[])
{
    // 一般不带参数(用 kCodeFilePath); 想换文件就传一个路径: test_agent_compress <file>
    const std::string fileOverride = (argc > 1) ? std::string(argv[1]) : std::string();

    // 无缓冲: 重定向到日志文件时也能实时看到进度(一轮可能几分钟)
    setvbuf(stdout, nullptr, _IONBF, 0);

    JZApplication app;
    CompressTest test;
    return test.run(fileOverride);
}
