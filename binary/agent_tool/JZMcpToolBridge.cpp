#include "JZMcpToolBridge.h"

#include "JZDebug.h"

using namespace std;

namespace
{

// MCP 工具结果对象: { content:[{type:"text",text:...}], isError:bool }
Json::Value mcpTextResult(const string &text, bool isError)
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

// JSON-RPC 的 id 可能是数字也可能是字符串, 统一成 map 的键
string mcpIdKey(const Json::Value &id)
{
    if (id.isString())
        return id.asString();
    if (id.isInt())
        return std::to_string(id.asInt());
    if (id.isInt64())
        return std::to_string(id.asInt64());
    if (id.isUInt())
        return std::to_string(id.asUInt());
    return id.toStyledString();
}

// args -> function_args 字符串。JZAgent 那一路的 ToolCall.function_args 是模型给的原始
// JSON 串, 工具可能读它, 这里补上免得桥接后的行为跟本地不一致。
string writeJson(const Json::Value &value)
{
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    builder["emitUTF8"] = true;
    return Json::writeString(builder, value);
}

} // namespace

bool jzToolDefineToMcpSchema(const ToolDefine &def, std::string &name,
                             std::string &description, Json::Value &inputSchema)
{
    name = def.name;
    description = def.description;

    Json::Value func = def.toJson();
    inputSchema = func["parameters"];

    // 没声明参数时 toJson 也会给出 {type:"object", properties:{}} —— 合法空 schema。
    // 这里兜一下, 免得 MCP 端拿到 null 而不是一个 object。
    if (!inputSchema.isObject())
    {
        inputSchema = Json::Value(Json::objectValue);
        inputSchema["type"] = "object";
        inputSchema["properties"] = Json::Value(Json::objectValue);
    }

    return !name.empty();
}

JZMcpToolBridge::JZMcpToolBridge(JZAgentTool *inner)
    : JZMcpTool(nullptr)
    , m_inner(inner)
{
    if (!m_inner)
    {
        jzError() << "JZMcpToolBridge: inner 不能为空";
        return;
    }
    if (m_inner->parent() != nullptr)
        jzWarning() << "JZMcpToolBridge: inner 已有 parent, 本类会接管它的所有权:" << m_inner->toolName();

    string name, description;
    Json::Value schema;
    if (!jzToolDefineToMcpSchema(m_inner->toolDefine(), name, description, schema))
        jzWarning() << "JZMcpToolBridge: inner 没有工具名, MCP 侧会看不到它";

    setName(name);
    setDescription(description);
    setInputSchema(schema);

    // inner 每次 call 都会回一条 sigResult, 在这里转成 MCP 响应
    connect(&m_inner->sigResult, this, &JZMcpToolBridge::onInnerResult);
}

JZMcpToolBridge::~JZMcpToolBridge()
{
    delete m_inner;   // 所有权在本类
    m_inner = nullptr;
}

void JZMcpToolBridge::run(const Json::Value &id, const Json::Value &args)
{
    if (!m_inner)
    {
        sigFinished.emit(id, mcpTextResult("桥接的工具不存在", true));
        return;
    }

    // MCP 的请求 id(JSON-RPC)与工具内部的调用 id(字符串)不是一回事, 这里建一条映射,
    // 结果/取消时再翻回去。同一工具的多次调用并发在途也没问题。
    string localId = "mcpb_" + std::to_string(++m_idGen);
    m_pending[localId] = id;

    ToolCall call;
    call.id = localId;
    call.type = "function";
    call.function_name = m_inner->toolName();
    call.params = args;
    call.function_args = writeJson(args);

    m_inner->call(call);
}

void JZMcpToolBridge::cancel(const Json::Value &id)
{
    // MCP 只给请求 id, 反查这条请求在本地用的 id 再转给 inner
    string key = mcpIdKey(id);
    for (auto it = m_pending.begin(); it != m_pending.end(); ++it)
    {
        if (mcpIdKey(it->second) != key)
            continue;

        string localId = it->first;
        // 先摘登记再让 inner 收尾: inner 之后若仍然回一条 sigResult,
        // onInnerResult 查不到登记会直接丢弃, 不会重复应答。
        m_pending.erase(it);
        if (m_inner)
            m_inner->cancel(localId);
        break;
    }

    // 与基类 JZMcpTool::cancel 一致: 无论有没有找到在途请求都回一条取消
    // (核依据 sigCanceled 回送取消响应)
    sigCanceled.emit(id);
}

void JZMcpToolBridge::onInnerResult(const ToolCallResult &result)
{
    auto it = m_pending.find(result.id);
    if (it == m_pending.end())
        return;   // 已取消(或已应答)的迟到结果, 丢弃

    Json::Value mcpId = it->second;
    m_pending.erase(it);

    string text = result.result.empty() ? string("(无输出)") : result.result;
    sigFinished.emit(mcpId, mcpTextResult(text, !result.isOk));
}
