#define _CRT_SECURE_NO_WARNINGS
#include <cmath>
#include <iostream>
#include <random>
#include <string>
#include <functional>

#include "JZAgent.h"
#include "JZAgentTool.h"
#include "JZApplication.h"
#include "JZEventLoop.h"
#include "JZTimer.h"
#include "JZCommandLineParser.h"
#include "JZDebug.h"

// 工具基类已统一为：call(const ToolCall&) 实现逻辑，并通过 sigResult 返回结果。
// 下面两个本地工具用于测试，等价于原 JZAgentFunctionTool / JZAgentAsyncFunctionTool。
class FunctionTool : public JZAgentTool
{
public:
    std::function<ToolCallResult(const ToolCall&)> function;
    void call(const ToolCall& params) override
    {
        sigResult.emit(function(params));
    }
};

class AsyncFunctionTool : public JZAgentTool
{
public:
    std::function<void(JZAgentTool*, const ToolCall&)> function;
    void call(const ToolCall& params) override
    {
        function(this, params);
    }
};

// ------------------------------------------------------------------
// 配置（通过命令行注入；未设置时使用占位默认值）
//   --modelUrl   例如 http://localhost:8000/v1/chat/completions
//   --modelName  例如 gpt-3.5-turbo
//   --modelToken 例如 sk-xxxx
// ------------------------------------------------------------------
static JZAgentConfig loadConfig(int argc, char **argv)
{
    JZAgentConfig cfg;
    //cfg.modelUrl = "http://127.0.0.1:5003/compatible-mode/v1/chat/completions";
    cfg.modelUrl = "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions";
    cfg.modelName = "qwen3.7-max-2026-06-08";
    cfg.modelToken = "sk-c7cbd6d56e5049c69a640f9596c978d3";
    cfg.rootPath  = "";
    cfg.maxRounds = 100;

    JZCommandLineParser parser;
    parser.setDescription("JZAgent 测试程序");
    parser.addOption("--modelUrl", &cfg.modelUrl);
    parser.addOption("--modelName", &cfg.modelName);
    parser.addOption("--modelToken", &cfg.modelToken);

    if (!parser.parse(argc, argv)) {
        jzError() << parser.errorString().c_str();
        parser.printHelp();
        return cfg;
    }
    return cfg;
}

class AgentTester : public JZObject
{
public:
    enum Step { Guess, Math, Role, Async, Done };

    AgentTester(JZEventLoop *loop, const JZAgentConfig &cfg)
        : m_loop(loop), m_config(cfg)
    {
        m_agent = nullptr;
        m_step = Guess;
        m_secret = 0;
        m_guessCount = 0;
        m_pollTimer = new JZTimer(this);
        connect(&m_pollTimer->sigTimeout, this, &AgentTester::onPoll);
    }

    void start()
    {
        jzDebug().nospace() << "==== Agent 测试启动 ====";
        jzDebug().nospace() << "modelUrl : " << m_config.modelUrl;
        jzDebug().nospace() << "modelName: " << m_config.modelName;
        jzDebug().nospace() << "=======================";
        runCurrentScenario();
    }

protected:
    // 启动当前场景（每个场景用一个全新的 JZAgent，避免工具互相干扰）
    void runCurrentScenario()
    {
        m_agent = new JZAgent();
        m_agent->setStreamEnabled(true);
        m_agent->init(m_config);

        connect(&m_agent->sigReply, this, &AgentTester::onReply);
        connect(&m_agent->sigError, this, &AgentTester::onError);
        connect(&m_agent->sigFinished, this, &AgentTester::onFinished);

        if (m_step == Guess)
            setupGuess();
        else if (m_step == Math)
            setupMath();
        else if (m_step == Role)
            setupRole();
        else
            setupAsync();

        m_pollTimer->start(300);
    }

    void setupGuess()
    {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<int> dis(1, 10000);
        m_secret = dis(gen);
        m_guessCount = 0;

        m_agent->setSystemPrompt(
            "你正在和测试程序玩猜数字游戏。程序心里想了一个 1 到 10000 之间的整数。\n"
            "请每次调用 guess 工具来提交你的猜测（不要直接在文字里猜）。\n"
            "程序会返回\"太大了\"、\"太小了\"或\"猜对了\"。\n"
            "请使用二分查找策略，尽快猜中。");

        ToolDefine def;
        def.name = "guess";
        def.description = "提交一个猜测的数字。返回结果会告诉你太大了、太小了还是猜对了。";
        ParamSchema p;
        p.name = "number";
        p.type = NUMBER;
        p.description = "你猜测的整数（范围 1~10000）";
        def.parameters.push_back(p);
        def.required_parameters.push_back("number");

        auto *tool = new FunctionTool();
        tool->setToolDefine(def);
        tool->function = [this](const ToolCall &call) -> ToolCallResult {
            ToolCallResult r;
            r.id = call.id;
            r.isOk = true;
            int n = call.params["number"].asInt();
            m_guessCount++;
            if (n > m_secret)
                r.result = "太大了，请往小猜。";
            else if (n < m_secret)
                r.result = "太小了，请往大猜。";
            else
                r.result = "猜对了！";
            return r;
        };
        m_agent->addTool(tool);

        jzDebug().nospace() << "[场景1 猜数字] 秘密数字已生成(1~10000)，请模型开始猜...";
        m_agent->request("我们开始玩猜数字游戏吧，请猜一个 1 到 10000 之间的整数。");
    }

    void setupMath()
    {
        auto makeMath = [](const std::string &name, const std::string &desc,
                          std::function<double(double, double)> op, bool isDiv) -> JZAgentTool* {
            ToolDefine def;
            def.name = name;
            def.description = desc;
            ParamSchema a;
            a.name = "a";
            a.type = NUMBER;
            a.description = "操作数 a";
            ParamSchema b;
            b.name = "b";
            b.type = NUMBER;
            b.description = "操作数 b";
            def.parameters.push_back(a);
            def.parameters.push_back(b);
            def.required_parameters.push_back("a");
            def.required_parameters.push_back("b");

            auto *tool = new FunctionTool();
            tool->setToolDefine(def);
            tool->function = [op, isDiv](const ToolCall &call) -> ToolCallResult {
                ToolCallResult r;
                r.id = call.id;
                r.isOk = true;
                double x = call.params["a"].asDouble();
                double y = call.params["b"].asDouble();
                if (isDiv && y == 0.0) {
                    r.isOk = false;
                    r.result = "除数不能为 0";
                    return r;
                }
                double v = op(x, y);
                if (std::isfinite(v) && std::floor(v) == v)
                    r.result = std::to_string((long long)v);
                else
                    r.result = std::to_string(v);
                return r;
            };
            return tool;
        };

        m_agent->setSystemPrompt(
            "你是一个严谨的计算助手。遇到数学运算时，请调用对应的工具进行计算，"
            "不要心算，最后用一句话给出结果。");

        m_agent->addTool(makeMath("add",      "两个数相加 a+b", [](double a, double b) { return a + b; }, false));
        m_agent->addTool(makeMath("subtract", "两个数相减 a-b", [](double a, double b) { return a - b; }, false));
        m_agent->addTool(makeMath("multiply", "两个数相乘 a*b", [](double a, double b) { return a * b; }, false));
        m_agent->addTool(makeMath("divide",   "两个数相除 a/b", [](double a, double b) { return a / b; }, true));

        jzDebug().nospace() << "[场景2 加减乘除] 提问: (123 + 456) * 7 / 3 = ?  (预期 1351)";
        m_agent->request("请帮我计算 (123 + 456) * 7 / 3 等于多少？请使用工具计算。");
    }

    void setupRole()
    {
        m_agent->setSystemPrompt(
            "你叫\"小智\"，是由 JZFramework 驱动的智能助手，性格友好且严谨。"
            "当用户问\"你是谁\"时，请明确回答你叫小智，并简要介绍自己。");

        jzDebug().nospace() << "[场景3 角色设定] 提问: 你是谁？";
        m_agent->request("你是谁？");
    }

    // 场景4：异步工具（AsyncFunctionTool）
    // 异步工具不在 function 中直接返回结果，而是自行在"完成后"通过
    // sigResult 把结果交还；Agent 会据此继续驱动对话。
    void setupAsync()
    {
        ToolDefine def;
        def.name = "asyncAdd";
        def.description = "异步加法工具：将两个数相加后返回，内部模拟一次异步后端调用（延迟后回传结果）。";
        ParamSchema a;
        a.name = "a";
        a.type = NUMBER;
        a.description = "操作数 a";
        ParamSchema b;
        b.name = "b";
        b.type = NUMBER;
        b.description = "操作数 b";
        def.parameters.push_back(a);
        def.parameters.push_back(b);
        def.required_parameters.push_back("a");
        def.required_parameters.push_back("b");

        auto *tool = new AsyncFunctionTool();
        tool->setToolDefine(def);
        tool->function = [](JZAgentTool *tool, const ToolCall &call) {
            double x = call.params["a"].asDouble();
            double y = call.params["b"].asDouble();
            // 模拟异步后端（如网络/IO）：延迟 500ms 后再把结果交还。
            JZTimer::singleShot(500, [tool, id = call.id, x, y]() {
                ToolCallResult r;
                r.id = id;
                r.isOk = true;
                double v = x + y;
                if (std::isfinite(v) && std::floor(v) == v)
                    r.result = std::to_string((long long)v);
                else
                    r.result = std::to_string(v);
                tool->sigResult.emit(r);
            });
        };
        m_agent->addTool(tool);

        m_agent->setSystemPrompt(
            "你是一个助手。当需要做两个数相加时，请调用 asyncAdd 工具，"
            "不要心算，等工具返回结果后用一句话给出答案。");

        jzDebug().nospace() << "[场景4 异步工具] 提问: 100 + 200 = ? (预期 300)";
        m_agent->request("请帮我用异步工具计算 100 + 200 等于多少？");
    }

    // 流式增量（仅作进度展示）
    void onReply(std::string text)
    {
        text = str_utf8_to_local(text);
        printf("%s", text.c_str());
    }

    void onError(std::string msg)
    {
        jzError() << msg.c_str();
        finishStep();
    }

    // 仅 stop()/error 路径会触发；正常完成走 onPoll。做防御性保护。
    void onFinished()
    {
        if (m_agent)
            finishStep();
    }

    // 轮询判断当前轮是否结束：历史最后一条是 Assistant 且不再请求工具
    void onPoll()
    {
        if (!m_agent)
            return;
        const LLMContext *ctx = m_agent->context();
        if (ctx->history.empty())
            return;
        const std::shared_ptr<LLMHistory> &last = ctx->history.back();
        if (last->role == LLMHistory::Assistant &&
            last->toolRequest.empty())
            finishStep();
    }

    int countToolMessages() const
    {
        int n = 0;
        for (const auto &h : m_agent->context()->history)
            if (h->role == LLMHistory::Tool)
                n++;
        return n;
    }

    void finishStep()
    {
        m_pollTimer->stop();
        if (!m_agent)
            return;

        const LLMContext *ctx = m_agent->context();
        std::string finalText;
        if (!ctx->history.empty() && ctx->history.back()->role == LLMHistory::Assistant)
            finalText = ctx->history.back()->content;

        if (m_step == Guess) {
            jzDebug().nospace() << "[场景1 结果] 秘密数字=" << m_secret
                      << "，模型共猜了 " << m_guessCount << " 次";
        }
        else if (m_step == Math) {
            jzDebug().nospace() << "[场景2 结果] 模型回答: " << finalText;
            jzDebug().nospace() << "[场景2 结果] 工具调用次数: " << countToolMessages();
        }
        else if (m_step == Role) {
            jzDebug().nospace() << "[场景3 结果] 模型回答: " << finalText;
            bool ok = finalText.find("小智") != std::string::npos;
            jzDebug().nospace() << "[场景3 结果] 关键词\"小智\"检查: " << (ok ? "PASS" : "FAIL");
        }
        else if (m_step == Async) {
            jzDebug().nospace() << "[场景4 结果] 模型回答: " << finalText;
            jzDebug().nospace() << "[场景4 结果] 工具调用次数: " << countToolMessages();
            bool ok = finalText.find("300") != std::string::npos;
            jzDebug().nospace() << "[场景4 结果] 关键词\"300\"检查: " << (ok ? "PASS" : "FAIL");
        }

        // 延迟到事件循环空闲再推进，避免与 sigError/sigFinished 同步级联冲突
        JZAgent *a = m_agent;
        m_agent = nullptr;
        a->deleteLater();
        JZTimer::singleShot(0, [this]() { advance(); });
    }

    void advance()
    {
        if (m_step == Guess) {
            m_step = Math;
            runCurrentScenario();
        }
        else if (m_step == Math) {
            m_step = Role;
            runCurrentScenario();
        }
        else if (m_step == Role) {
            m_step = Async;
            runCurrentScenario();
        }
        else {
            jzDebug().nospace() << "==== 全部测试完成 ====";
            m_loop->quit();
        }
    }

    JZEventLoop *m_loop;
    JZAgentConfig m_config;
    JZAgent *m_agent;
    JZTimer *m_pollTimer;
    Step m_step;
    int m_secret;
    int m_guessCount;
};

int main(int argc, char *argv[])
{
    JZApplication app;
    JZEventLoop loop;

    AgentTester tester(&loop, loadConfig(argc, argv));
    tester.start();

    return loop.exec();
}
