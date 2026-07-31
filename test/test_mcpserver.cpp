#include <string>
#include <cmath>
#include <stdexcept>

#include "JZApplication.h"
#include "JZDebug.h"
#include "JZConsoleInput.h"
#include "JZMcpServer.h"
#include "JZMcpCore.h"
#include "JZMcpTool.h"
#include "json/json.h"

using namespace std;

// 构造一个描述参数的 JSON Schema 辅助函数
static Json::Value makeSchema(std::initializer_list<std::pair<std::string, std::string>> props,
                              std::initializer_list<std::string> required)
{
    Json::Value schema(Json::objectValue);
    schema["type"] = "object";

    Json::Value properties(Json::objectValue);
    for (const auto& p : props)
    {
        Json::Value field(Json::objectValue);
        field["type"] = p.second;   // "string" / "number" / ...
        properties[p.first] = field;
    }
    schema["properties"] = properties;

    Json::Value req(Json::arrayValue);
    for (const auto& r : required)
        req.append(r);
    schema["required"] = req;

    return schema;
}

// 构造 MCP 文本结果对象: { content:[{type:"text",text:...}], isError:bool }
static Json::Value makeTextResult(const std::string& text, bool isError = false)
{
    Json::Value result(Json::objectValue);
    Json::Value content(Json::arrayValue);
    Json::Value item(Json::objectValue);
    item["type"] = "text";
    item["text"] = text;
    content.append(item);
    result["content"] = content;
    result["isError"] = isError;
    return result;
}

// ----------------------------------------------------------------------
// 工具1: echo —— 回显文本
class EchoTool : public JZMcpTool
{
public:
    EchoTool(JZObject* parent = nullptr) : JZMcpTool(parent)
    {
        setName("echo");
        setDescription("回显输入的文本");
        setInputSchema(makeSchema({{"text", "string"}}, {"text"}));
    }
    void run(const Json::Value& id, const Json::Value& args) override
    {
        sigFinished.emit(id, makeTextResult(args.get("text", "").asString()));
    }
};

// 工具2: add —— 两数相加
class AddTool : public JZMcpTool
{
public:
    AddTool(JZObject* parent = nullptr) : JZMcpTool(parent)
    {
        setName("add");
        setDescription("计算两个数字之和");
        setInputSchema(makeSchema({{"a", "number"}, {"b", "number"}}, {"a", "b"}));
    }
    void run(const Json::Value& id, const Json::Value& args) override
    {
        double a = args.get("a", 0.0).asDouble();
        double b = args.get("b", 0.0).asDouble();
        sigFinished.emit(id, makeTextResult(std::to_string(a + b)));
    }
};

// 工具3: pow —— 幂运算, 演示错误处理
class PowTool : public JZMcpTool
{
public:
    PowTool(JZObject* parent = nullptr) : JZMcpTool(parent)
    {
        setName("pow");
        setDescription("计算 base 的 exponent 次幂");
        setInputSchema(makeSchema({{"base", "number"}, {"exponent", "number"}}, {"base", "exponent"}));
    }
    void run(const Json::Value& id, const Json::Value& args) override
    {
        if (!args.isMember("base") || !args.isMember("exponent"))
        {
            sigFinished.emit(id, makeTextResult("base 与 exponent 均为必填", true));  // isError = true
            return;
        }
        double base = args["base"].asDouble();
        double exp = args["exponent"].asDouble();
        sigFinished.emit(id, makeTextResult(std::to_string(std::pow(base, exp))));
    }
};

int main(int argc, char* argv[])
{
    JZApplication app;

    int httpPort = 3000;
    int wsPort   = 3001;
    if (argc > 1)
        httpPort = atoi(argv[1]);
    if (argc > 2)
        wsPort = atoi(argv[2]);

    JZMcpServer server;

    // 协议核心: 注册 serverInfo 与工具(均通过 core())
    server.core()->setServerInfo("JZMcpServer", "1.0.0");

    // 以 JZMcpTool 子类对象形式注册(生命周期由核心接管)
    server.core()->registerTool(new EchoTool(server.core()));
    server.core()->registerTool(new AddTool(server.core()));
    server.core()->registerTool(new PowTool(server.core()));

    // 后端1: HTTP(Streamable HTTP)
    if (!server.listenHttp(httpPort))
    {
        jzError() << "HTTP 服务启动失败";
        return 1;
    }

    // 后端2: WebSocket
    if (!server.listenWebSocket(wsPort))
    {
        jzError() << "WebSocket 服务启动失败";
        return 1;
    }

    jzInfo() << "JZMcpServer 已启动:";
    jzInfo() << "  HTTP      端点: POST http://127.0.0.1:" << httpPort << "/mcp";
    jzInfo() << "  WebSocket 端点: ws://127.0.0.1:" << wsPort;

    // 后端3: 纯文本(示例代码, 实际使用时可把任意字符串喂入 processText, 结果通过回调异步返回)
    // 2026-07-28: 每请求需在 _meta 携带协议版本(无 initialize 握手)。
    // server.processText(R"({"jsonrpc":"2.0","id":1,"method":"tools/list","_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}})",
    //     [](bool hasResponse, const std::string& outJson) {
    //         if (hasResponse) jzInfo() << "RESP:" << outJson;
    //     });

    jzInfo() << "按 Ctrl+C 退出";

    // 后端3: 终端输入 —— 将每行文本作为 JSON-RPC 请求喂入 processText
    JZConsoleInput consoleInput(&server);
    consoleInput.setPrompt("mcp> ");
    connect(&consoleInput.sigReceived, [&server](const std::string& line) {
        std::string text = line;
        // 去掉首尾空白
        size_t b = text.find_first_not_of(" \t\r\n");
        size_t e = text.find_last_not_of(" \t\r\n");
        if (b == std::string::npos)
            return;
        text = text.substr(b, e - b + 1);

        server.processText(text,
            [](bool hasResponse, const std::string& outJson) {
                if (hasResponse)
                    jzInfo() << "RESP:" << outJson;
            });
    });
    consoleInput.start();

    int ret = app.exec();

    consoleInput.stop();
    return ret;
}
