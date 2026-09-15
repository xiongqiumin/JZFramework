// JZMcpToolBridge 冒烟测试
//
// 不起窗口、不监听端口: 直接把 JSON-RPC 文本喂给 JZMcpServer::processText,
// 走完整链路 JZAgentTool -> JZMcpToolBridge -> JZMcpCore -> 响应。
// 覆盖: 元数据转换(tools/list) / 同步结果 / 异步结果 / isOk->isError / 并发调用的 id 映射。
// 期望输出: ALL PASS

#include <stdio.h>
#include <string>

#include "JZAgentTool.h"
#include "JZApplication.h"
#include "JZMcpCore.h"
#include "JZMcpServer.h"
#include "JZMcpToolBridge.h"
#include "JZTimer.h"
#include "json/json.h"

using namespace std;

// ==========================================================================
// 被测的"普通工具": 全都是 JZAgentTool 派生 —— 桥的另一头
// ==========================================================================

static ParamSchema makeParam(const string &name, ParamType type, const string &desc)
{
    ParamSchema p;
    p.name = name;
    p.type = type;
    p.description = desc;
    return p;
}

// 同步回显
class BridgeEchoTool : public JZAgentTool
{
public:
    BridgeEchoTool()
    {
        ToolDefine def;
        def.name = "br_echo";
        def.description = "回显文本(桥接测试)";
        def.parameters.push_back(makeParam("text", ParamType::String, "要回显的文本"));
        def.required_parameters.push_back("text");
        setToolDefine(def);
    }

    void call(const ToolCall &params) override
    {
        ToolCallResult r;
        r.id = params.id;
        r.isOk = true;
        r.result = params.params.get("text", "").asString();
        sigResult.emit(r);
    }
};

// 异步返回: 结果晚一点才来, 验证桥接的异步结果通道与 id 映射
class BridgeSlowTool : public JZAgentTool
{
public:
    BridgeSlowTool()
    {
        ToolDefine def;
        def.name = "br_slow";
        def.description = "延迟一会儿再回(验证异步结果通道)";
        def.parameters.push_back(makeParam("text", ParamType::String, "文本"));
        setToolDefine(def);
    }

    void call(const ToolCall &params) override
    {
        string id = params.id;
        string text = params.params.get("text", "").asString();
        JZTimer::singleShot(50, this, [this, id, text]()
        {
            ToolCallResult r;
            r.id = id;
            r.isOk = true;
            r.result = text + "-done";
            sigResult.emit(r);
        });
    }
};

// 失败: isOk=false 应该变成 MCP 的 isError=true
class BridgeFailTool : public JZAgentTool
{
public:
    BridgeFailTool()
    {
        ToolDefine def;
        def.name = "br_fail";
        def.description = "永远失败(验证 isError)";
        setToolDefine(def);
    }

    void call(const ToolCall &params) override
    {
        ToolCallResult r;
        r.id = params.id;
        r.isOk = false;
        r.result = "故意失败";
        sigResult.emit(r);
    }
};

// ==========================================================================
// 测试驱动
// ==========================================================================

static int g_fail = 0;

static void check(bool ok, const string &what)
{
    printf("%s %s\n", ok ? "[ok]  " : "[FAIL]", what.c_str());
    if (!ok)
        g_fail++;
}

static Json::Value parseJson(const string &text)
{
    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(text, root))
        return Json::Value(Json::objectValue);
    return root;
}

// 2026-07-28: 没有 initialize 握手, 每请求要在 _meta 里带协议版本
static string request(int id, const string &method, const string &paramsJson)
{
    string body = "{\"jsonrpc\":\"2.0\",\"id\":" + to_string(id) + ",\"method\":\"" + method + "\"";
    if (!paramsJson.empty())
        body += ",\"params\":" + paramsJson;
    body += ",\"_meta\":{\"io.modelcontextprotocol/protocolVersion\":\"2026-07-28\"}}";
    return body;
}

static const Json::Value *findTool(const Json::Value &tools, const string &name)
{
    if (!tools.isArray())
        return nullptr;
    for (const Json::Value &t : tools)
    {
        if (t.get("name", "").asString() == name)
            return &t;
    }
    return nullptr;
}

class BridgeTest
{
public:
    int run()
    {
        m_server.core()->setServerInfo("agent_tool_test", "1.0.0");
        m_server.core()->registerTool(new JZMcpToolBridge(new BridgeEchoTool()));
        m_server.core()->registerTool(new JZMcpToolBridge(new BridgeSlowTool()));
        m_server.core()->registerTool(new JZMcpToolBridge(new BridgeFailTool()));

        stepList();

        // 兜底: 万一某个回调没来, 10 秒后退出并判失败(而不是挂住)
        JZTimer::singleShot(10000, [this]()
        {
            check(false, "超时: 有步骤的回调没有触发");
            jzApp->quit();
        });

        return jzApp->exec();
    }

private:
    // ---- 1. 元数据: ToolDefine -> MCP 的 name/description/inputSchema ----
    void stepList()
    {
        m_server.processText(request(1, "tools/list", ""), [this](bool has, const string &out)
        {
            check(has, "tools/list 有响应");
            Json::Value resp = parseJson(out);
            const Json::Value &tools = resp["result"]["tools"];
            check(tools.isArray() && (int)tools.size() == 3, "tools/list 列出 3 个桥接工具");

            const Json::Value *echo = findTool(tools, "br_echo");
            check(echo != nullptr, "tools/list 里有 br_echo(名字转换正确)");
            if (echo)
            {
                check((*echo)["description"].asString() == "回显文本(桥接测试)", "description 转换正确");

                const Json::Value &schema = (*echo)["inputSchema"];
                check(schema.isObject() && schema.get("type", "").asString() == "object",
                      "inputSchema.type == object");
                check(schema["properties"]["text"].get("type", "").asString() == "string",
                      "inputSchema.properties.text.type == string");
                check(schema["properties"]["text"].get("description", "").asString() == "要回显的文本",
                      "参数 description 保留");
                check(schema["required"].isArray() &&
                          !schema["required"].empty() &&
                          schema["required"][0].asString() == "text",
                      "required 保留");
            }

            stepEcho();
        });
    }

    // ---- 2. 同步结果 ----
    void stepEcho()
    {
        m_server.processText(request(2, "tools/call", R"({"name":"br_echo","arguments":{"text":"hello"}})"),
                             [this](bool has, const string &out)
        {
            check(has, "br_echo 有响应");
            Json::Value resp = parseJson(out);
            const Json::Value &result = resp["result"];
            check(result["content"][0].get("text", "").asString() == "hello",
                  "同步结果文本原样回传");
            check(!result["isError"].asBool(), "isOk=true -> isError=false");
            stepSlow();
        });
    }

    // ---- 3. 异步结果: 工具晚 50ms 才 emit sigResult ----
    void stepSlow()
    {
        m_server.processText(request(3, "tools/call", R"({"name":"br_slow","arguments":{"text":"slow"}})"),
                             [](bool has, const string &out)
        {
            check(has, "br_slow 有响应");
            Json::Value resp = parseJson(out);
            const Json::Value &result = resp["result"];
            check(result["content"][0].get("text", "").asString() == "slow-done",
                  "异步结果经桥接回到 MCP 响应");
        });
        // stepFail 与 br_slow 无关, 可以紧接着发(两条请求并发在途)
        stepFail();
    }

    // ---- 4. 失败映射 ----
    void stepFail()
    {
        m_server.processText(request(4, "tools/call", R"({"name":"br_fail","arguments":{}})"),
                             [this](bool has, const string &out)
        {
            check(has, "br_fail 有响应");
            Json::Value resp = parseJson(out);
            const Json::Value &result = resp["result"];
            check(result["isError"].asBool(), "isOk=false -> isError=true");
            check(result["content"][0].get("text", "").asString() == "故意失败", "失败原因文本回传");
            stepConcurrent();
        });
    }

    // ---- 5. 同一个工具的两次调用并发在途: 各自拿到自己的结果 ----
    void stepConcurrent()
    {
        m_pendingConcurrent = 2;
        m_server.processText(request(5, "tools/call", R"({"name":"br_slow","arguments":{"text":"A"}})"),
                             [this](bool has, const string &out)
        {
            Json::Value resp = parseJson(out);
            check(resp["id"].asInt() == 5, "并发调用 A 的响应 id == 5");
            check(resp["result"]["content"][0].get("text", "").asString() == "A-done",
                  "并发调用 A 拿到自己的结果");
            if (--m_pendingConcurrent == 0)
                finish();
        });
        m_server.processText(request(6, "tools/call", R"({"name":"br_slow","arguments":{"text":"B"}})"),
                             [this](bool has, const string &out)
        {
            Json::Value resp = parseJson(out);
            check(resp["id"].asInt() == 6, "并发调用 B 的响应 id == 6");
            check(resp["result"]["content"][0].get("text", "").asString() == "B-done",
                  "并发调用 B 拿到自己的结果");
            if (--m_pendingConcurrent == 0)
                finish();
        });
    }

    void finish()
    {
        jzApp->quit();
    }

    JZMcpServer m_server;
    int m_pendingConcurrent = 0;
};

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    // 输出重定向到管道时 stdout 是全缓冲的, 中途崩了会一个字都看不到 —— 关掉缓冲
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("[step] main\n");

    JZApplication app;
    printf("[step] app ready, dir=%s\n", app.applicationDirPath().c_str());

    BridgeTest test;
    printf("[step] bridge test ready\n");

    int ret = test.run();

    printf("\n%s (fail=%d)\n", g_fail == 0 ? "ALL PASS" : "FAILED", g_fail);
    return g_fail == 0 ? ret : 1;
}
