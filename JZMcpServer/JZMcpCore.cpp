#include "JZMcpCore.h"
#include "JZMcpTool.h"

#include <stdexcept>
#include <memory>
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
// 协议版本登记表 —— 对齐官方 SDK(mcp_types/version.py):
//   HANDSHAKE(走 initialize 握手) = 2024-11-05, 2025-03-26, 2025-06-18, 2025-11-25
//   MODERN  (无状态, 版本走每请求 _meta)                          = 2026-07-28
// ⚠ 客户端是"拿自己的版本号来协商"的, 规则照抄官方(_probe.py + 官方 server 实测):
//   - initialize 握手: 要的版本在 HANDSHAKE 表里 → 原样回显; 否则**回自己最新的握手版本**
//     (2025-11-25) 做反报价, 不是报错 —— 官方 server 对 "2026-07-28 的 initialize" 就是回
//     2025-11-25。报 -32022 会把整档客户端挡死(我们踩过: CodeBuddy 报的就是它)。
//   - 每请求 _meta: 要的版本既不在 HANDSHAKE 也不等于 MODERN 才回 -32022(带 data.supported
//     全表)。客户端靠这个列表判断"换现代协议 / 退回握手 / 真的不兼容", 所以列表要全。
//   - server/discover 的结果必须带 supportedVersions / cacheScope / ttlMs, 否则客户端把结果
//     反序列化成 DiscoverResult 失败 ⇒ 判定"不是现代 server" ⇒ 退回 initialize(见 onDiscover)。
static const char* kModernProtocolVersion  = "2026-07-28";   // MODERN: 无状态, 版本走每请求 _meta
static const char* kLatestHandshakeVersion = "2025-11-25";   // HANDSHAKE 里最新, 用作反报价
static const char* kLegacyProtocolVersion  = "2025-06-18";
static const char* kOldProtocolVersion     = "2025-03-26";
static const char* kOldestProtocolVersion  = "2024-11-05";

// 2026-07-18 起移除 initialize 握手: 协议版本/能力改走每请求 _meta 命名空间。
const char* JZMcpCore::kMetaProtocolVersion   = "io.modelcontextprotocol/protocolVersion";
const char* JZMcpCore::kMetaClientCapabilities = "io.modelcontextprotocol/clientCapabilities";
const char* JZMcpCore::kMetaServerInfo         = "io.modelcontextprotocol/serverInfo";
const char* JZMcpCore::kMetaClientInfo         = "io.modelcontextprotocol/clientInfo";

// 握手时代的版本(initialize 只能协商这几个; 顺序无关)
const char* const JZMcpCore::kHandshakeProtocolVersions[] = {
    kOldestProtocolVersion,
    kOldProtocolVersion,
    kLegacyProtocolVersion,
    kLatestHandshakeVersion,
};

// 全部支持的版本 = 握手时代 + 现代, 只用于 -32022 的 data.supported 回显
const char* const JZMcpCore::kSupportedProtocolVersions[] = {
    kModernProtocolVersion,
    kLatestHandshakeVersion,
    kLegacyProtocolVersion,
    kOldProtocolVersion,
    kOldestProtocolVersion,
};

// 2026-07-28 错误码(替代旧版 -32602 + supported)
static const int kUnsupportedProtocolVersion = -32022;

// 取请求里的"信封" _meta。⚠ 2026-07-28 规范把它放在 **params._meta**(官方 python/ts 客户端
// 就是这么发的: params={'_meta': {'io.modelcontextprotocol/protocolVersion': '2026-07-28', ...}});
// 早期实现(和我们自己的旧客户端)放在请求顶层。两处都认, 否则会把现代请求当 2025 请求处理
// —— 具体表现: 结果里不带 resultType/cacheScope/ttlMs, 客户端反序列化校验直接失败。
static const Json::Value& requestMeta(const Json::Value& request)
{
    static const Json::Value kNull;
    if (request.isMember("params") && request["params"].isObject()
        && request["params"].isMember("_meta"))
        return request["params"]["_meta"];
    if (request.isMember("_meta"))
        return request["_meta"];
    return kNull;
}

JZMcpCore::JZMcpCore(JZObject* parent)
    : JZObject(parent)
    , m_serverName("JZMcpServer")
    , m_serverVersion("1.0.0")
    , m_protocolVersion(kModernProtocolVersion)
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
    // 没带 requestId 的取消通知无从匹配, 直接忽略
    if (requestId.isNull())
        return;

    auto it = m_running.find(idKey(requestId));
    if (it == m_running.end())
        return;     // 请求已结束(响应都回送了)或压根不存在: 无需处理, 客户端会忽略后续响应

    // 先让工具停下来(如 terminal_run 杀掉在跑的命令)
    JZMcpTool* tool = it->second.tool;
    ProcessCallback cb = it->second.cb;
    // 擦掉在途记录: 工具随后若再 emit sigFinished/sigCanceled, onToolSignal 找不到就直接丢弃,
    // 不会再给这个已取消的请求补一个响应。
    m_running.erase(it);
    tool->cancel(requestId);

    // 按 MCP 规范(2025-06-18 Cancellation): 被取消的请求**不再回送响应**, 由客户端本地
    // 按 -32800 处理。但传输层必须给这次请求一个"结束", 否则对端那个 POST 会一直挂着 ——
    // 所以这里回一个"无响应体"(HTTP 端表现为空 202 / 直接结束 SSE 流)。
    if (cb)
        cb(false, std::string(), true);
}

void JZMcpCore::onCancel(const Json::Value& params)
{
    if (!params.isMember("requestId"))
        return;
    cancelRequest(params["requestId"]);
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

    // 流式通知: 在请求结束前多次回送(不结束请求)
    connect(&tool->sigNotify, [=](const Json::Value& id, const Json::Value& notification) {
        onToolNotify(tool, id, notification);
    });
}

void JZMcpCore::onToolSignal(JZMcpTool* tool, const Json::Value& id, const Json::Value& result, bool canceled)
{
    std::string key = idKey(id);
    auto it = m_running.find(key);
    if (it == m_running.end())
        return;

    // 先取出需要的字段再 erase: 之后再解引用迭代器就是野指针了
    ProcessCallback cb = std::move(it->second.cb);
    McpFormat fmt = it->second.fmt;
    m_running.erase(it);

    if (!cb)
        return;
    if (canceled)
        cb(true, writeJson(makeError(id, kRequestCancelled, "Request canceled")), true);
    else
        cb(true, writeJson(makeResult(id, result, fmt)), true);
}

void JZMcpCore::onToolNotify(JZMcpTool* tool, const Json::Value& id, const Json::Value& notification)
{
    std::string key = idKey(id);
    auto it = m_running.find(key);
    if (it == m_running.end())
        return;                     // 请求已结束(或不存在), 丢弃通知

    ProcessCallback cb = it->second.cb;
    if (!cb)
        return;
    // finished=false: 流式中间分块, 传输层保持连接(如 POST 内联 SSE)
    cb(true, writeJson(notification), false);
}

void JZMcpCore::runToolAsync(int toolId, McpFormat fmt, const Json::Value& id,
                             const Json::Value& args, ProcessCallback cb)
{
    const Tool* tool = findTool(toolId);
    if (!tool)
    {
        if (cb)
            cb(true, writeJson(makeError(id, kInvalidParams, "Unknown tool")), true);
        return;
    }

    std::string key = idKey(id);

    PendingRequest pr;
    pr.tool = tool->tool;
    pr.id = id;
    pr.cb = cb;
    pr.fmt = fmt;
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
        cb(true, writeJson(makeError(Json::Value::null, kParseError, "Parse error")), true);
        return true;
    }

    // JSON-RPC 批量请求(数组): 按规范应回一个 JSON 数组响应, 不能一条一回 ——
    // HTTP 一次请求只有一个响应, 逐条回送的话只有第一条能到客户端(且会污染 keep-alive 连接)。
    if (root.isArray())
    {
        // 批里的异步项(tools/call)不是在 dispatch 里同步返回的, 而是稍后经信号回调回送。
        // 这些响应不能各自回送(一次 HTTP 请求只有一个响应), 得先攒着, 等全批都完成再
        // 合成一个数组一起发 —— 所以这里给整批包一层共享上下文。
        struct BatchContext
        {
            Json::Value responses = Json::Value(Json::arrayValue);
            int pending = 0;            // 还没回送的异步项
            bool closed = false;        // 循环是否已走完(决定何时可以发)
            ProcessCallback cb;
        };
        auto ctx = std::make_shared<BatchContext>();
        ctx->cb = cb;

        auto flushIfDone = [ctx]() {
            if (!ctx->closed || ctx->pending > 0 || !ctx->cb)
                return;
            if (!ctx->responses.empty())
                ctx->cb(true, writeJson(ctx->responses), true);
            else
                ctx->cb(false, std::string(), true);   // 全是通知: 无响应体
        };

        // 传给 dispatch 的回调: 只有异步项会走这里(同步响应是 dispatch 的返回值)。
        ProcessCallback batchCb = [ctx, flushIfDone](bool hasResponse, const std::string& outJson, bool finished) {
            if (!finished)
                return;     // 批量语义下不做逐帧流式, 中间帧丢弃
            if (hasResponse)
            {
                Json::Value v;
                if (parseJson(outJson, v))
                    ctx->responses.append(v);
            }
            if (ctx->pending > 0)
                ctx->pending--;
            flushIfDone();
        };

        for (const auto& item : root)
        {
            bool isNotification = false;
            bool asyncHandled = false;

            // 记录派发前已收集到的条数: 工具若在 run() 里同步 emit 完成信号,
            // 响应在 dispatch 返回前就已经进来了, 这时不能再算作"未完成的异步项"。
            const Json::ArrayIndex before = ctx->responses.size();
            Json::Value resp = dispatch(item, isNotification, batchCb, asyncHandled);

            if (asyncHandled)
            {
                if (!isNotification && ctx->responses.size() == before)
                    ctx->pending++;     // 确实是异步(尚未回送), 等它回来再发
                continue;
            }
            if (!isNotification)
                ctx->responses.append(resp);
        }

        // 循环结束: 没有异步项时这里就把整批发出去; 有异步项则由它们的回调在最后触发。
        // (若某个工具既不 emit sigFinished 也不 emit sigCanceled, 该批会一直挂着 —— 工具必须给个结果。)
        ctx->closed = true;
        flushIfDone();
        return true;
    }

    // 单个请求
    bool isNotification = false;
    bool asyncHandled = false;
    Json::Value resp = dispatch(root, isNotification, cb, asyncHandled);
    if (asyncHandled)
    {
        // 通知(如 notifications/initialized / notifications/cancelled): 没有响应体,
        // 但必须显式告诉传输层"这次请求结束了", 否则 HTTP 端一直不回包, 客户端会挂到超时。
        if (isNotification)
            cb(false, std::string(), true);
        return true;
    }
    if (isNotification)
    {
        cb(false, std::string(), true);
        return true;
    }

    cb(true, writeJson(resp), true);
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

    // 判定该请求对应的响应格式(2025 还是 2026)。2025 客户端不携带 _meta, 由此区分。
    McpFormat fmt = detectFormat(request);

    // 若请求显式带了 _meta 协议版本, 必须受支持; 版本不兼容返回
    // UnsupportedProtocolVersionError(-32022)。无 _meta(2025 客户端)放行。
    Json::Value verErr = checkProtocolVersion(request);
    if (!verErr.isNull())
    {
        verErr["id"] = id;     // 带上请求 id, 便于客户端匹配
        return verErr;
    }

    // ---- 通知类 ----
    if (method.rfind("notifications/", 0) == 0)
    {
        // notifications/cancelled: 取消对应的在途请求(后续由 sigCanceled 回送响应)
        // notifications/initialized: 2025-06-18 握手完成通知, 忽略即可(无状态)。
        if (method == "notifications/cancelled")
            onCancel(params);
        isNotification = true;
        asyncHandled = true;   // 不立即回送
        return Json::Value::null;
    }

    // ---- 请求类 ----
    if (method == "initialize")
    {
        // 标准握手(2024-11-05 ~ 2025-11-25)。版本协商放在 onInitialize 里: 要的版本在
        // 握手表里就回显, 否则回自己最新的握手版本做反报价 —— **不报错**(官方 server 就是
        // 这么干的: 对 2026-07-28 的 initialize 回 2025-11-25)。报 -32022 会把整档客户端
        // 挡死, 客户端接不接受反报价由它自己决定(不支持就断开)。
        isNotification = false;
        return makeResult(id, onInitialize(params), McpFormat::V2025);
    }
    if (method == "ping")
    {
        // 心跳探测(MCP utilities): 成功回一个空 result, 用来验证对端还活着。
        isNotification = false;
        return makeResult(id, Json::Value(Json::objectValue), fmt);
    }
    if (method == "server/discover")
    {
        // 2026-07-28 无状态发现接口: 暴露版本/能力/身份(无需 _meta 也可调用, 用于探测)
        isNotification = false;
        return makeResult(id, onDiscover(), McpFormat::V2026);
    }
    if (method == "tools/list")
    {
        isNotification = false;
        return makeResult(id, onToolsList(), fmt);
    }
    if (method == "tools/call")
    {
        isNotification = false;
        onToolsCall(fmt, id, params, cb, asyncHandled);
        return Json::Value::null;   // 响应由信号回调异步回送
    }

    isNotification = false;
    return makeError(id, kMethodNotFound, "Method not found: " + method);
}

Json::Value JZMcpCore::checkProtocolVersion(const Json::Value& request) const
{
    // 2026-07-28: 协议版本由每请求 _meta[kMetaProtocolVersion] 携带, 无握手。
    // 若 _meta 显式声明了版本, 必须受支持; 否则(无 _meta, 即握手时代客户端)放行。
    const Json::Value& meta = requestMeta(request);
    std::string clientVersion;
    if (meta.isObject() && meta.isMember(kMetaProtocolVersion))
        clientVersion = meta[kMetaProtocolVersion].asString();

    if (!clientVersion.empty() && !isSupportedVersion(clientVersion))
    {
        Json::Value data(Json::objectValue);
        Json::Value supported(Json::arrayValue);
        for (const char* v : kSupportedProtocolVersions)
            supported.append(v);
        data["supported"] = supported;
        data["requested"] = clientVersion;
        return makeErrorData(Json::Value::null, kUnsupportedProtocolVersion,
                             "Unsupported protocol version: " + clientVersion, data);
    }
    return Json::Value::null;
}

JZMcpCore::McpFormat JZMcpCore::detectFormat(const Json::Value& request) const
{
    // _meta 中 protocolVersion == "2026-07-28" → V2026; 其余(含无 _meta 的握手客户端) → V2025。
    const Json::Value& meta = requestMeta(request);
    if (meta.isObject() && meta.isMember(kMetaProtocolVersion)
        && meta[kMetaProtocolVersion].asString() == kModernProtocolVersion)
        return McpFormat::V2026;
    return McpFormat::V2025;
}

bool JZMcpCore::isSupportedVersion(const std::string& v) const
{
    for (const char* s : kSupportedProtocolVersions)
        if (v == s)
            return true;
    return false;
}

bool JZMcpCore::isHandshakeVersion(const std::string& v) const
{
    for (const char* s : kHandshakeProtocolVersions)
        if (v == s)
            return true;
    return false;
}

Json::Value JZMcpCore::onInitialize(const Json::Value& params) const
{
    // 标准握手(2024-11-05 ~ 2025-11-25)。
    const std::string requested = params.get("protocolVersion", "").asString();

    // 回显协商出的协议版本: 客户端要的是握手版本就回显它; 否则(没带版本 / 带了个不是握手
    // 时代的版本, 例如把现代的 2026-07-28 塞进 initialize)回自己最新的握手版本做反报价 ——
    // 客户端若不支持这个版本, 按规范由它自行断开。
    Json::Value result(Json::objectValue);
    result["protocolVersion"] = isHandshakeVersion(requested) ? requested
                                                              : std::string(kLatestHandshakeVersion);

    Json::Value capabilities(Json::objectValue);
    Json::Value toolsCap(Json::objectValue);
    toolsCap["listChanged"] = false;
    capabilities["tools"] = toolsCap;
    result["capabilities"] = capabilities;

    Json::Value serverInfo(Json::objectValue);
    serverInfo["name"] = m_serverName;
    serverInfo["version"] = m_serverVersion;
    result["serverInfo"] = serverInfo;

    // 2025-06-18: initialize 响应的可选服务器说明
    if (!m_instructions.empty())
        result["instructions"] = m_instructions;

    return result;
}

Json::Value JZMcpCore::onDiscover() const
{
    // 替代旧版 initialize: 暴露服务器支持的协议版本/能力/身份(2026-07-28 无状态发现)。
    // ⚠ 字段必须照官方 DiscoverResult 给全 —— 客户端(官方 python/ts client)拿到结果会先
    //   反序列化成 DiscoverResult, 缺 supportedVersions / cacheScope 就直接解析失败 ⇒
    //   判定"这台不是现代 server" ⇒ 退回 initialize 握手。我们之前缺这两个字段, 于是
    //   每个客户端都被平白降级到握手时代(实测官方 client 就是这样退回的)。
    //   supportedVersions 只列**现代(无状态)**版本: 客户端会从里面挑一个用于后续请求,
    //   混进握手版本会被挑走。
    Json::Value result(Json::objectValue);
    Json::Value supported(Json::arrayValue);
    supported.append(m_protocolVersion);   // m_protocolVersion = 本服务器讲的现代版本
    result["supportedVersions"] = supported;
    result["cacheScope"] = "private";      // 结果含主机名/工作区等信息, 不跨授权上下文共享
    result["ttlMs"] = 0;                   // 不做缓存

    Json::Value capabilities(Json::objectValue);
    Json::Value toolsCap(Json::objectValue);
    toolsCap["listChanged"] = false;
    capabilities["tools"] = toolsCap;
    result["capabilities"] = capabilities;

    Json::Value serverInfo(Json::objectValue);
    serverInfo["name"] = m_serverName;
    serverInfo["version"] = m_serverVersion;
    result["serverInfo"] = serverInfo;

    // 2026-07-28: 可选服务器说明
    if (!m_instructions.empty())
        result["instructions"] = m_instructions;

    return result;
}

Json::Value JZMcpCore::withServerMeta(const Json::Value& result) const
{
    // 2026-07-28: 每个 result 的 _meta 中附带服务器身份。
    Json::Value out = result;
    Json::Value meta(Json::objectValue);
    if (out.isObject() && out.isMember("_meta"))
        meta = out["_meta"];
    Json::Value serverInfo(Json::objectValue);
    serverInfo["name"] = m_serverName;
    serverInfo["version"] = m_serverVersion;
    meta[kMetaServerInfo] = serverInfo;
    out["_meta"] = meta;
    return out;
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

void JZMcpCore::onToolsCall(McpFormat fmt, const Json::Value& id, const Json::Value& params,
                             ProcessCallback cb, bool& asyncHandled)
{
    asyncHandled = true;   // 响应交由信号回调回送

    const std::string name = params.get("name", "").asString();
    if (name.empty())
    {
        if (cb)
            cb(true, writeJson(makeError(id, kInvalidParams, "Missing tool name")), true);
        return;
    }

    const Tool* tool = findTool(name);
    if (!tool)
    {
        if (cb)
            cb(true, writeJson(makeError(id, kInvalidParams, "Unknown tool: " + name)), true);
        return;
    }

    const Json::Value arguments = params.isMember("arguments") ? params["arguments"]
                                                              : Json::Value(Json::objectValue);

    // 提交执行(连接完成信号, run() 内通过信号回送结果), 携带协商出的响应格式
    runToolAsync(tool->toolId, fmt, id, arguments, cb);
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
Json::Value JZMcpCore::makeResult(const Json::Value& id, const Json::Value& result, McpFormat fmt)
{
    Json::Value resp(Json::objectValue);
    resp["jsonrpc"] = "2.0";
    resp["id"] = id;

    if (fmt == McpFormat::V2026)
    {
        // 2026-07-28: 所有 result 必须带 resultType("complete" / "input_required");
        // 列表类结果(tools/list / server/discover / prompts/list / resources/list ...)还
        // **必须**带 cacheScope —— 客户端会拿该版本的 schema 反序列化校验, 少一个字段整条
        // 请求就报错(实测官方 client: "unhandled errors in a TaskGroup")。所有 result 再在
        // _meta 中附带服务器身份 serverInfo。
        Json::Value res = result;
        if (!res.isMember("resultType"))
            res["resultType"] = "complete";
        if (!res.isMember("cacheScope"))
            res["cacheScope"] = "private";
        if (!res.isMember("ttlMs"))
            res["ttlMs"] = 0;          // 0 = 不缓存(cacheScope/ttlMs 都是客户端校验的必填项)
        resp["result"] = withServerMeta(res);
    }
    else
    {
        // 2025-06-18: 标准格式, 不附加 resultType / _meta.serverInfo。
        resp["result"] = result;
    }
    return resp;
}

Json::Value JZMcpCore::makeError(const Json::Value& id, int code, const std::string& message)
{
    return makeErrorData(id, code, message, Json::Value::null);
}

Json::Value JZMcpCore::makeErrorData(const Json::Value& id, int code,
                                     const std::string& message, const Json::Value& data)
{
    Json::Value resp(Json::objectValue);
    resp["jsonrpc"] = "2.0";
    resp["id"] = id;
    Json::Value error(Json::objectValue);
    error["code"] = code;
    error["message"] = message;
    if (!data.isNull())
        error["data"] = data;
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
