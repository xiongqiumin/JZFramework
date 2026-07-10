#include "JZMcpCore.h"
#include "JZMcpTool.h"

#include <stdexcept>
#include <map>

#include "JZDebug.h"

// JSON-RPC 标准错误码
static const int kParseError     = -32700;
static const int kInvalidRequest = -32600;
static const int kMethodNotFound = -32601;
static const int kInvalidParams  = -32602;
static const int kInternalError  = -32603;
static const int kRequestCancelled = -32800;  // MCP: 请求被取消

// ======================================================================
// JZMcpCore
// ======================================================================
JZMcpCore::JZMcpCore(JZObject* parent)
    : JZObject(parent)
    , m_serverName("JZMcpServer")
    , m_serverVersion("1.0.0")
    , m_protocolVersion("2024-11-05")
    , m_nextToolId(0)
{
}

JZMcpCore::~JZMcpCore()
{
}

void JZMcpCore::setServerInfo(const std::string& name, const std::string& version)
{
    m_serverName = name;
    m_serverVersion = version;
}

int JZMcpCore::registerTool(JZMcpTool* tool)
{
    if (!tool)
        return -1;

    // 生命周期由本核心接管
    tool->setParent(this);

    Tool t;
    t.toolId = m_nextToolId++;
    t.tool = tool;
    m_tools.push_back(std::move(t));

    // 注册时即完成信号连接(每个工具只连一次, 使用 AutoConnection)
    ensureConnected(tool);
    return t.toolId;
}

void JZMcpCore::cancelRequest(const Json::Value& requestId)
{
    auto it = m_running.find(idKey(requestId));
    if (it != m_running.end())
        it->second.tool->cancel(requestId);
}

void JZMcpCore::onCancel(const Json::Value& params)
{
    const Json::Value& rid = params["requestId"];
    cancelRequest(rid);
}

// ----------------------------------------------------------------------
void JZMcpCore::ensureConnected(JZMcpTool* tool)
{
    if (m_toolConnected.count(tool))
        return;
    m_toolConnected.insert(tool);

    // 每个工具只连接一次完成信号(框架的 disconnect 接口存在缺陷, 故不重复连接/断开)
    connect(&tool->sigFinished, [=](const Json::Value& id, const Json::Value& result) {
        onToolSignal(tool, id, result, false);
    });

    connect(&tool->sigCanceled, [=](const Json::Value& id) {
        onToolSignal(tool, id, Json::Value::null, true);
    });
}

void JZMcpCore::onToolSignal(JZMcpTool* tool, const Json::Value& id, const Json::Value& result, bool canceled)
{
    std::string key = idKey(id);
    auto it = m_running.find(key);
    if (it == m_running.end())
        return;

    ProcessCallback cb = std::move(it->second.cb);
    m_running.erase(it);

    if (!cb)
        return;
    if (canceled)
        cb(true, writeJson(makeError(id, kRequestCancelled, "Request canceled")));
    else
        cb(true, writeJson(makeResult(id, result)));
}

void JZMcpCore::runToolAsync(int toolId, const Json::Value& id, const Json::Value& args, ProcessCallback cb)
{
    const Tool* tool = findTool(toolId);
    if (!tool)
    {
        if (cb)
            cb(true, writeJson(makeError(id, kInvalidParams, "Unknown tool")));
        return;
    }

    std::string key = idKey(id);

    PendingRequest pr;
    pr.tool = tool->tool;
    pr.id = id;
    pr.cb = cb;
    m_running[key] = std::move(pr);

    // 每个工具只连接一次完成信号
    ensureConnected(tool->tool);

    // 提交执行: run() 内部通过 sigFinished / sigCanceled 之一回送(信号携带请求 id)。
    tool->tool->run(id, args);
}

// ----------------------------------------------------------------------
bool JZMcpCore::process(const std::string& body, ProcessCallback cb)
{
    if (!cb)
        return false;

    Json::Value root;
    if (!parseJson(body, root))
    {
        cb(true, writeJson(makeError(Json::Value::null, kParseError, "Parse error")));
        return true;
    }

    // JSON-RPC 批量请求(数组)
    if (root.isArray())
    {
        for (const auto& item : root)
        {
            bool isNotification = false;
            bool asyncHandled = false;
            Json::Value resp = dispatch(item, isNotification, cb, asyncHandled);
            if (asyncHandled)
                continue;               // 响应稍后由信号回调回送
            if (!isNotification)
                cb(true, writeJson(resp));
        }
        return true;
    }

    // 单个请求
    bool isNotification = false;
    bool asyncHandled = false;
    Json::Value resp = dispatch(root, isNotification, cb, asyncHandled);
    if (asyncHandled)
        return true;
    if (isNotification)
    {
        cb(false, std::string());
        return true;
    }

    cb(true, writeJson(resp));
    return true;
}

// ----------------------------------------------------------------------
Json::Value JZMcpCore::dispatch(const Json::Value& request, bool& isNotification,
                                ProcessCallback cb, bool& asyncHandled)
{
    isNotification = false;
    asyncHandled = false;

    if (!request.isObject())
        return makeError(Json::Value::null, kInvalidRequest, "Invalid Request");

    const std::string method = request.get("method", "").asString();
    Json::Value id = request.isMember("id") ? request["id"] : Json::Value::null;
    const Json::Value params = request.isMember("params") ? request["params"]
                                                          : Json::Value(Json::objectValue);

    // 无 id 视为通知
    isNotification = !request.isMember("id");

    if (method.empty())
    {
        isNotification = false;
        return makeError(id, kInvalidRequest, "Missing method");
    }

    // ---- 通知类 ----
    if (method == "notifications/initialized" ||
        method.rfind("notifications/", 0) == 0)
    {
        // notifications/cancelled: 取消对应的在途请求(后续由 sigCanceled 回送响应)
        if (method == "notifications/cancelled")
            onCancel(params);
        isNotification = true;
        asyncHandled = true;   // 不立即回送
        return Json::Value::null;
    }

    // ---- 请求类 ----
    if (method == "initialize")
    {
        isNotification = false;
        return makeResult(id, onInitialize(params));
    }
    if (method == "ping")
    {
        isNotification = false;
        return makeResult(id, Json::Value(Json::objectValue));
    }
    if (method == "tools/list")
    {
        isNotification = false;
        return makeResult(id, onToolsList());
    }
    if (method == "tools/call")
    {
        isNotification = false;
        onToolsCall(id, params, cb, asyncHandled);
        return Json::Value::null;   // 响应由信号回调异步回送
    }

    isNotification = false;
    return makeError(id, kMethodNotFound, "Method not found: " + method);
}

Json::Value JZMcpCore::onInitialize(const Json::Value& params)
{
    // 尽量回显客户端请求的协议版本
    std::string proto = params.get("protocolVersion", m_protocolVersion).asString();
    if (proto.empty())
        proto = m_protocolVersion;

    Json::Value result(Json::objectValue);
    result["protocolVersion"] = proto;

    Json::Value capabilities(Json::objectValue);
    Json::Value toolsCap(Json::objectValue);
    toolsCap["listChanged"] = false;
    capabilities["tools"] = toolsCap;
    result["capabilities"] = capabilities;

    Json::Value serverInfo(Json::objectValue);
    serverInfo["name"] = m_serverName;
    serverInfo["version"] = m_serverVersion;
    result["serverInfo"] = serverInfo;

    return result;
}

Json::Value JZMcpCore::onToolsList()
{
    Json::Value tools(Json::arrayValue);
    for (const auto& t : m_tools)
    {
        Json::Value item(Json::objectValue);
        item["name"] = t.tool->name();
        item["description"] = t.tool->description();
        item["inputSchema"] = t.tool->inputSchema();
        tools.append(item);
    }

    Json::Value result(Json::objectValue);
    result["tools"] = tools;
    return result;
}

void JZMcpCore::onToolsCall(const Json::Value& id, const Json::Value& params,
                             ProcessCallback cb, bool& asyncHandled)
{
    asyncHandled = true;   // 响应交由信号回调回送

    const std::string name = params.get("name", "").asString();
    if (name.empty())
    {
        if (cb)
            cb(true, writeJson(makeError(id, kInvalidParams, "Missing tool name")));
        return;
    }

    const Tool* tool = findTool(name);
    if (!tool)
    {
        if (cb)
            cb(true, writeJson(makeError(id, kInvalidParams, "Unknown tool: " + name)));
        return;
    }

    const Json::Value arguments = params.isMember("arguments") ? params["arguments"]
                                                              : Json::Value(Json::objectValue);

    // 提交执行(连接完成信号, run() 内通过信号回送结果)
    runToolAsync(tool->toolId, id, arguments, cb);
}

// ----------------------------------------------------------------------
const JZMcpCore::Tool* JZMcpCore::findTool(const std::string& name) const
{
    for (const auto& t : m_tools)
    {
        if (t.tool->name() == name)
            return &t;
    }
    return nullptr;
}

const JZMcpCore::Tool* JZMcpCore::findTool(int toolId) const
{
    for (const auto& t : m_tools)
    {
        if (t.toolId == toolId)
            return &t;
    }
    return nullptr;
}

// ----------------------------------------------------------------------
Json::Value JZMcpCore::makeResult(const Json::Value& id, const Json::Value& result)
{
    Json::Value resp(Json::objectValue);
    resp["jsonrpc"] = "2.0";
    resp["id"] = id;
    resp["result"] = result;
    return resp;
}

Json::Value JZMcpCore::makeError(const Json::Value& id, int code, const std::string& message)
{
    Json::Value resp(Json::objectValue);
    resp["jsonrpc"] = "2.0";
    resp["id"] = id;
    Json::Value error(Json::objectValue);
    error["code"] = code;
    error["message"] = message;
    resp["error"] = error;
    return resp;
}

bool JZMcpCore::parseJson(const std::string& text, Json::Value& out)
{
    if (text.empty())
        return false;

    Json::CharReaderBuilder builder;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    std::string errs;
    const char* begin = text.data();
    const char* end = begin + text.size();
    return reader->parse(begin, end, &out, &errs);
}

std::string JZMcpCore::writeJson(const Json::Value& value)
{
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";           // 紧凑输出
    builder["emitUTF8"] = true;            // 不转义 UTF-8
    return Json::writeString(builder, value);
}

std::string JZMcpCore::idKey(const Json::Value& id)
{
    if (id.isNull())
        return "null";
    return writeJson(id);
}
