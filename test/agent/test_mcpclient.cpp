// JZMcpClient 冒烟测试: 分别用 Streamable HTTP 和 stdio 子进程两种方式连上
// JZMcpServer, 检查远端工具被拉到本地, 并通过 JZAgentTool 接口真正调用一次。
//
// 用法:
//   test_mcpclient.exe            跑完整测试
//   test_mcpclient.exe --stdio    作为 stdio MCP 服务器运行(供上一个用例拉起)

#include <functional>
#include <iostream>
#include <string>

#include "JZAgent.h"
#include "JZAgentTool.h"
#include "JZApplication.h"
#include "JZDebug.h"
#include "JZMcpClient.h"
#include "JZMcpCore.h"
#include "JZMcpServer.h"
#include "JZMcpTool.h"
#include "JZPlatform.h"
#include "JZTimer.h"
#include "json/json.h"

using namespace std;

// ----------------------------------------------------------------------
// 服务端工具: add —— 两数相加
class AddTool : public JZMcpTool
{
public:
    AddTool() : JZMcpTool(nullptr)
    {
        setName("add");
        setDescription("计算两个数字之和");

        Json::Value schema(Json::objectValue);
        schema["type"] = "object";
        Json::Value props(Json::objectValue);
        Json::Value a(Json::objectValue), b(Json::objectValue);
        a["type"] = "number";
        b["type"] = "number";
        props["a"] = a;
        props["b"] = b;
        schema["properties"] = props;
        Json::Value required(Json::arrayValue);
        required.append("a");
        required.append("b");
        schema["required"] = required;
        setInputSchema(schema);
    }

    void run(const Json::Value& id, const Json::Value& args) override
    {
        double sum = args.get("a", 0.0).asDouble() + args.get("b", 0.0).asDouble();

        Json::Value result(Json::objectValue);
        Json::Value content(Json::arrayValue);
        Json::Value item(Json::objectValue);
        item["type"] = "text";
        item["text"] = to_string(sum);
        content.append(item);
        result["content"] = content;

        sigFinished.emit(id, result);
    }
};

// 真实大模型(vLLM OpenAI 兼容接口), 可用 --modelUrl / --modelName 覆盖, --no-llm 跳过
static const char* kDefaultModelUrl  = "http://172.17.160.147:8000/v1/chat/completions";
static const char* kDefaultModelName = "/workspace/RedHatAI/Qwen3.6-35B-A3B-NVFP4";

// ----------------------------------------------------------------------
// stdio MCP 服务器: 一行一条 JSON-RPC, 响应写回 stdout
static int runStdioServer()
{
    JZApplication app;

    JZMcpServer server;
    server.core()->setServerInfo("StdioMcpServer", "1.0.0");
    server.core()->registerTool(new AddTool());

    string line;
    while (std::getline(std::cin, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;

        bool hasResponse = false;
        string out;
        server.processText(line, [&](bool has, const string& text)
        {
            hasResponse = has;
            out = text;
        });

        if (hasResponse && !out.empty())
        {
            std::cout << out << "\n";
            std::cout.flush();
        }
    }
    return 0;
}

// ----------------------------------------------------------------------
int main(int argc, char* argv[])
{
    if (argc > 1 && string(argv[1]) == "--stdio")
        return runStdioServer();

    string model_url = kDefaultModelUrl;
    string model_name = kDefaultModelName;
    bool with_llm = true;
    for (int i = 1; i < argc; i++)
    {
        string arg = argv[i];
        if (arg == "--modelUrl" && i + 1 < argc)     model_url = argv[++i];
        else if (arg == "--modelName" && i + 1 < argc) model_name = argv[++i];
        else if (arg == "--no-llm")                  with_llm = false;
    }

    JZApplication app;

    // ---- HTTP 服务端 ----
    const int port = 3888;
    JZMcpServer server;
    server.core()->setServerInfo("TestMcpServer", "1.0.0");
    server.core()->registerTool(new AddTool());
    if (!server.listenHttp(port))
    {
        jzError() << "HTTP 服务启动失败, port =" << port;
        return 1;
    }

    // ---- 客户端1: Streamable HTTP ----
    JZMcpClient http_client;
    string url = "http://127.0.0.1:" + to_string(port) + "/mcp";
    if (!http_client.startHttp(url))
    {
        jzError() << "HTTP 连接失败:" << http_client.errorString();
        return 1;
    }
    jzInfo() << "[http] 已连接:" << http_client.serverName() << http_client.serverVersion()
             << ", 工具数 =" << (int)http_client.tools().size();

    // ---- 客户端2: stdio 子进程(拉起本程序的 --stdio 模式) ----
    JZMcpClient stdio_client;
    string exe = JZPlatform::applicationFilePath();
    if (!stdio_client.startProcess(exe, { string("--stdio") }))
    {
        jzError() << "stdio 连接失败:" << stdio_client.errorString();
        return 1;
    }
    jzInfo() << "[stdio] 已连接:" << stdio_client.serverName() << stdio_client.serverVersion()
             << ", 工具数 =" << (int)stdio_client.tools().size();

    // 工具定义是否被正确翻译(本地名带 server 前缀, 故按远端名反查)
    auto findTool = [](JZMcpClient* client, const string& remote)
    {
        for (auto* t : client->tools())
        {
            if (client->remoteToolName(t->toolName()) == remote)
                return t;
        }
        return (JZAgentTool*)nullptr;
    };

    for (auto* client : { &http_client, &stdio_client })
    {
        JZAgentTool* tool = findTool(client, "add");
        if (!tool)
        {
            jzError() << "没有拉到 add 工具";
            return 1;
        }
        jzInfo() << "工具 " << tool->toolName() << " -> add:"
                 << tool->toolDefine().description
                 << ", 参数数 =" << (int)tool->toolDefine().parameters.size()
                 << ", 必填数 =" << (int)tool->toolDefine().required_parameters.size();
    }

    // ---- addMcp: agent 接管 client 所有权, 并把远端工具登记进来 ----
    JZAgent* agent = new JZAgent(&app);
    JZMcpClient* mcp = new JZMcpClient();
    if (!mcp->startHttp(url))
    {
        jzError() << "[addMcp] 连接失败:" << mcp->errorString();
        return 1;
    }
    agent->addMcp(mcp);

    JZAgentTool* mcp_tool = findTool(mcp, "add");
    if (!mcp_tool || agent->tool(mcp_tool->toolName()) != mcp_tool)
    {
        jzError() << "[addMcp] 远端工具没有登记到 agent";
        return 1;
    }
    jzInfo() << "[addMcp] agent 已登记远端工具:" << mcp_tool->toolName()
             << mcp_tool->toolDefine().description;

    int result = 1;

    // 依次调用两个客户端的 add 工具: 2 + 3 应得到 "5.000000"
    auto callAdd = [&findTool](JZMcpClient* client, std::function<void(bool, const string&)> done)
    {
        JZAgentTool* tool = findTool(client, "add");
        connect(&tool->sigResult, [done](const ToolCallResult& res)
        {
            done(res.isOk, res.result);
        });

        ToolCall call;
        call.id = "1";
        call.function_name = tool->toolName();   // 用带前缀的本地名
        call.params["a"] = 2;
        call.params["b"] = 3;
        tool->call(call);
    };

    auto check = [](bool ok, const string& text)
    {
        return ok && text.compare(0, 1, "5") == 0;
    };

    // 真实大模型阶段: 模型自己决定调用 MCP 工具(add)来完成任务
    string llm_reply;
    bool llm_pass = false;

    auto runLlmStage = [&](std::function<void()> done)
    {
        JZAgentConfig cfg;
        cfg.modelUrl = model_url;
        cfg.modelName = model_name;
        cfg.modelToken = "EMPTY";        // vLLM 默认不鉴权
        cfg.maxRounds = 8;
        cfg.netTimeout = 180000;
        cfg.maxToken = 150000;      // 模型支持 160k 上下文
        cfg.tokenStrategy = JZAgentConfig::TokenTruncate;

        // 该服务端只给 512 生成额度, 且默认开启思考会挤占正文, 显式关掉
        Json::Value extra(Json::objectValue);
        extra["max_tokens"] = 512;
        Json::Value tpl(Json::objectValue);
        tpl["enable_thinking"] = false;
        extra["chat_template_kwargs"] = tpl;
        cfg.extraBody = extra;

        JZAgent* llm_agent = new JZAgent(&app);
        llm_agent->init(cfg);
        llm_agent->setSystemPrompt("你是一个助手。需要算加法时必须调用 add 工具, 不要自己口算。");

        JZMcpClient* llm_mcp = new JZMcpClient();
        if (!llm_mcp->startHttp(url))
        {
            jzError() << "[llm] MCP 连接失败:" << llm_mcp->errorString();
            delete llm_mcp;
            delete llm_agent;
            done();
            return;
        }
        llm_agent->addMcp(llm_mcp);
        jzInfo() << "[llm] 模型:" << model_name << ", 已接入工具:"
                 << llm_mcp->tools().size() << " 个";

        connect(&llm_agent->sigReply, [&llm_reply](const string& text)
        {
            llm_reply += text;
        });
        // 结束后只阻断信号: llm_agent 是 app 的子对象, 由 app 析构统一回收,
        // 这里再 deleteLater 会与父对象的子对象清理重复删除。
        connect(&llm_agent->sigError, [llm_agent, done](const string& err)
        {
            llm_agent->blockSignals(true);
            jzError() << "[llm] 错误:" << err;
            done();
        });
        connect(&llm_agent->sigFinished, [llm_agent, &llm_reply, &llm_pass, done]()
        {
            llm_agent->blockSignals(true);
            jzInfo() << "[llm] 回复:" << llm_reply;
            llm_pass = llm_reply.find("579") != string::npos;
            done();
        });

        llm_agent->request("用 add 工具算一下 123 加 456 等于多少, 最后只回答这个数字。");
    };

    JZTimer::singleShot(0, [&]()
    {
        callAdd(&http_client, [&](bool ok, const string& text)
        {
            jzInfo() << "[http] add 返回: isOk =" << ok << ", result =" << text;
            bool pass_http = check(ok, text);

            callAdd(&stdio_client, [&](bool ok2, const string& text2)
            {
                jzInfo() << "[stdio] add 返回: isOk =" << ok2 << ", result =" << text2;
                result = (pass_http && check(ok2, text2)) ? 0 : 1;

                if (!with_llm)
                {
                    app.quit();
                    return;
                }

                runLlmStage([&]()
                {
                    // 大模型阶段: 回复里出现 579 才算真的用上了 add 工具
                    if (!llm_pass)
                    {
                        jzError() << "[llm] 模型没有通过 add 工具给出 579";
                        result = 1;
                    }
                    app.quit();
                });
            });
        });
    });

    // 兜底超时, 避免卡死(真实模型 + 多轮工具调用需要留足时间)
    JZTimer::singleShot(180000, [&]()
    {
        jzError() << "调用超时";
        result = 1;
        app.quit();
    });

    app.exec();

    // agent 拥有 client 与代理工具: 只需删除 agent
    delete agent;
    jzInfo() << (result == 0 ? "测试通过" : "测试失败");
    return result;
}
