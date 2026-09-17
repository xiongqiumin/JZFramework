#include "JZMcpClient.h"

#include <cctype>
#include <set>
#include <sstream>

#include "JZApplication.h"
#include "JZByteArray.h"
#include "JZDebug.h"
#include "JZEventLoop.h"
#include "JZNetworkAccessManager.h"
#include "JZNetworkReply.h"
#include "JZNetworkRequest.h"
#include "JZProcess.h"
#include "JZStd.h"
#include "JZThread.h"
#include "JZTimer.h"
#include "JZUrl.h"

// 协议版本: 与 JZMcpCore 一致, 只支持无状态的 2026-07-28(无 initialize 握手)
static const char *kProtocolVersion = "2026-07-28";

static const char *kMetaProtocolVersion = "io.modelcontextprotocol/protocolVersion";
static const char *kMetaClientInfo = "io.modelcontextprotocol/clientInfo";

static const int kClientError = -32000;      // 本地错误(超时 / 连接不可用)

// ==================== JSON 工具 ====================
static bool mcpParseJson(const std::string &text, Json::Value &out)
{
    if (text.empty())
        return false;

    Json::CharReaderBuilder builder;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    std::string errs;
    const char *begin = text.data();
    const char *end = begin + text.size();
    return reader->parse(begin, end, &out, &errs);
}

static std::string mcpWriteJson(const Json::Value &value)
{
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    builder["emitUTF8"] = true;
    return Json::writeString(builder, value);
}

static Json::Value mcpMakeError(int code, const std::string &message)
{
    Json::Value err(Json::objectValue);
    err["code"] = code;
    err["message"] = message;
    return err;
}

// MCP 工具结果 -> 纯文本(content 数组里的 text 片段拼接)
static std::string mcpContentToText(const Json::Value &result)
{
    const Json::Value &content = result["content"];
    std::string text;
    if (content.isArray())
    {
        for (const auto &c : content)
        {
            if (c.isObject() && c.get("type", "").asString() == "text")
                text += c.get("text", "").asString();
            else if (c.isString())
                text += c.asString();
            else
                text += mcpWriteJson(c);
            text += "\n";
        }
    }
    else if (content.isString())
    {
        text = content.asString();
    }
    else if (result.isMember("structuredContent"))
    {
        text = mcpWriteJson(result["structuredContent"]);
    }
    else
    {
        text = mcpWriteJson(result);
    }
    return text;
}

// 远端 inputSchema(JSON Schema) -> JZAgent 的 ToolDefine
static void schemaToToolDefine(const Json::Value &schema, ToolDefine &def)
{
    const Json::Value &props = schema["properties"];
    if (props.isObject())
    {
        for (const std::string &key : props.getMemberNames())
        {
            ParamSchema p;
            p.name = key;
            if (p.fromJson(props[key]))
                def.parameters.push_back(p);
        }
    }

    const Json::Value &required = schema["required"];
    if (required.isArray())
    {
        for (const auto &r : required)
            def.required_parameters.push_back(r.asString());
    }
}

// ==================== JZMcpClientTool ====================
JZMcpClientTool::JZMcpClientTool(JZMcpClient *client)
    : m_client(client)
{
}

void JZMcpClientTool::call(const ToolCall &params)
{
    ToolCallResult result;
    result.id = params.id;

    if (!m_client || !m_client->isReady())
    {
        result.isOk = false;
        result.result = "MCP 连接不可用: " + toolName();
        sigResult.emit(result);
        return;
    }

    // 远端工具名不保存在代理里, 每次从 client 的映射表查
    std::string remote_name = m_client->remoteToolName(toolName());
    if (remote_name.empty())
    {
        result.isOk = false;
        result.result = "MCP 工具未登记: " + toolName();
        sigResult.emit(result);
        return;
    }

    // call 需要异步等待, 故保存一份请求上下文交给回调
    ToolCall call = params;
    m_client->callTool(remote_name, params.params,
                       [this, call](bool ok, const Json::Value &value)
    {
        onCallDone(call, ok, value);
    });
}

void JZMcpClientTool::onCallDone(const ToolCall &call, bool ok, const Json::Value &value)
{
    ToolCallResult result;
    result.id = call.id;

    if (!ok)
    {
        // 直接按服务器返回的内容上报失败(不猜测"服务器已变更")。
        result.isOk = false;
        result.result = "MCP 调用失败(" + toolName() + "): " + value.get("message", "").asString();
    }
    else
    {
        result.isOk = !value.get("isError", false).asBool();
        result.result = mcpContentToText(value);
    }

    if (result.result.empty())
        result.result = "(无输出)";

    sigResult.emit(result);
}

// ==================== JZMcpClient ====================
JZ_OBJECT_IMPL(JZMcpClient, JZObject)

JZMcpClient::JZMcpClient(JZObject *parent)
    : JZObject(parent), sigFinished(this)
{
}

JZMcpClient::~JZMcpClient()
{
    close();
}

bool JZMcpClient::startProcess(const std::string &program, const std::vector<std::string> &arguments,
                               int timeoutMs)
{
    if (!prepareConnect())
        return false;

    m_transport = Process;
    m_timeoutMs = timeoutMs;

    std::string prog = program;
#ifdef _WIN32
    // CreateProcess 的命令行里, 含空格的可执行文件路径必须带引号
    if (prog.find(' ') != std::string::npos && prog.front() != '"')
        prog = "\"" + prog + "\"";
#endif

    m_process = new JZProcess(this);
    connect(&m_process->sigReadyReadStandardOutput, this, &JZMcpClient::onProcessOutput);
    connect(&m_process->sigFinished, this, [this](int code, JZProcess::ExitStatus)
    {
        // 进程退出 = 本次连接流程失败(连接已丢失), 走永久失败并由 sigFinished 通知调用方
        setFailed("MCP 进程退出, 退出码 " + std::to_string(code));
    });

    if (!m_process->start(prog, arguments))
    {
        m_error = "启动 MCP 进程失败: " + program;
        close();
        return false;
    }

    // 异步: 发起后立即返回, 连接流程结束由 sigFinished 通知(结果看 isReady())
    beginNegotiate();
    return true;
}

bool JZMcpClient::startHttp(const std::string &url, int timeoutMs)
{
    if (!prepareConnect())
        return false;

    m_transport = Http;
    m_url = url;
    m_timeoutMs = timeoutMs;
    m_net = new JZNetworkAccessManager(this);

    beginNegotiate();
    return true;
}

void JZMcpClient::close()
{
    clearPending(true);
    m_ready = false;
    m_transport = None;
    m_readBuffer.clear();

    // 取消挂起的请求超时定时器由 finishPending 处理; 此处仅释放传输资源
    if (m_process)
    {
        // 主动关闭也会触发进程退出等信号, 屏蔽以免误发 sigError
        m_process->blockSignals(true);
        m_process->killProcess();
        m_process->deleteLater();
        m_process = nullptr;
    }
    if (m_net)
    {
        m_net->blockSignals(true);
        m_net->deleteLater();
        m_net = nullptr;
    }
}

void JZMcpClient::waitForFinished()
{
    if (m_finished)
        return;

    // 嵌套跑一个局部事件循环驱动连接流程, sigFinished 一到就退出。
    // loop 作为连接接收者: 析构时 guardPointer 自动失效, 之后不会再回调它。
    JZEventLoop loop;
    connect(&sigFinished, &loop, &JZEventLoop::quit);
    if (!m_finished)
        loop.exec();
}

bool JZMcpClient::isReady() const
{
    return m_ready;
}

JZMcpClient::Transport JZMcpClient::transport() const
{
    return m_transport;
}

const std::string &JZMcpClient::errorString() const
{
    return m_error;
}

const std::vector<JZAgentTool*> &JZMcpClient::tools() const
{
    return m_tools;
}

JZAgentTool *JZMcpClient::tool(const std::string &name) const
{
    for (auto *t : m_tools)
    {
        if (t->toolName() == name)
            return t;
    }
    return nullptr;
}

void JZMcpClient::setToolPrefix(const std::string &prefix)
{
    m_toolPrefix = prefix;
}

const std::string &JZMcpClient::toolPrefix() const
{
    return m_toolPrefix;
}

// 默认前缀: 服务器名 + "__", 只保留函数名合法字符 [A-Za-z0-9_-]
std::string JZMcpClient::makeLocalName(const std::string &remoteName) const
{
    std::string prefix = m_toolPrefix;
    if (prefix.empty())
    {
        std::string name = m_serverName.empty() ? std::string("mcp") : m_serverName;
        for (char c : name)
            prefix += (isalnum((unsigned char)c) || c == '_' || c == '-') ? c : '_';
        prefix += "__";
    }
    return prefix + remoteName;
}

std::string JZMcpClient::remoteToolName(const std::string &localName) const
{
    auto it = m_remoteNames.find(localName);
    return (it == m_remoteNames.end()) ? std::string() : it->second;
}

void JZMcpClient::setCallTimeout(int ms)
{
    m_callTimeoutMs = ms;
}

int JZMcpClient::callTimeout() const
{
    return m_callTimeoutMs;
}

std::string JZMcpClient::serverName() const
{
    return m_serverName;
}

std::string JZMcpClient::serverVersion() const
{
    return m_serverVersion;
}

// ==================== 请求 / 响应 ====================
Json::Value JZMcpClient::makeRequest(const std::string &method, const Json::Value &params,
                                     const std::string &protocolVersion)
{
    Json::Value req(Json::objectValue);
    req["jsonrpc"] = "2.0";
    req["method"] = method;
    if (!params.isNull())
        req["params"] = params;

    // 2026-07-28: 协议版本与客户端身份走每请求的 _meta
    Json::Value meta(Json::objectValue);
    meta[kMetaProtocolVersion] = protocolVersion.empty() ? std::string(kProtocolVersion) : protocolVersion;
    Json::Value clientInfo(Json::objectValue);
    clientInfo["name"] = "JZMcpClient";
    clientInfo["version"] = "1.0.0";
    meta[kMetaClientInfo] = clientInfo;
    req["_meta"] = meta;

    req["id"] = m_nextId++;
    return req;
}

int JZMcpClient::sendRequest(const std::string &method, const Json::Value &params, ReplyCallback cb,
                             int timeoutMs, const std::string &protocolVersion)
{
    Json::Value req = makeRequest(method, params, protocolVersion);
    int id = req["id"].asInt();

    JZTimer *timer = new JZTimer(this);
    timer->setSingleShot(true);
    connect(&timer->sigTimeout, this, [this, id]()
    {
        auto it = m_pending.find(id);
        if (it == m_pending.end())
            return;

        ReplyCallback cb = it->second.cb;
        JZTimer *t = it->second.timer;
        m_pending.erase(it);
        if (t)
        {
            t->stop();
            t->deleteLater();
        }
        if (cb)
            cb(false, mcpMakeError(kClientError, "请求超时"));
    });

    Pending p;
    p.cb = cb;
    p.timer = timer;
    m_pending[id] = p;
    timer->start(timeoutMs);

    if (!sendRaw(mcpWriteJson(req)))
    {
        finishPending(id, false, mcpMakeError(kClientError, "发送请求失败"));
        return -1;
    }
    return id;
}

bool JZMcpClient::sendRaw(const std::string &text)
{
    if (m_transport == Process)
    {
        if (!m_process || m_process->state() == JZProcess::NotRunning)
            return false;
        // stdio: 一行一条 JSON-RPC
        return m_process->write(str_toUtf8(text + "\n")) > 0;
    }
    else if (m_transport == Http)
    {
        JZNetworkRequest req;
        req.setUrl(JZUrl(m_url));
        req.setHeader(JZNetworkRequest::ContentTypeHeader, "application/json");
        req.setRawHeader("Accept", "application/json, text/event-stream");

        JZNetworkReply *reply = m_net->post(req, str_toUtf8(text));
        if (!reply)
            return false;

        connect(&reply->sigFinished, this, [this, reply]() { onHttpReply(reply); });
        reply->setTimeOut(m_callTimeoutMs + 5000);
        return true;
    }

    return false;
}

void JZMcpClient::onRecvText(const std::string &text)
{
    Json::Value msg;
    if (mcpParseJson(text, msg))
    {
        handleMessage(msg);
        return;
    }

    // Streamable HTTP 可能返回 SSE: 逐行取 data: 负载
    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.compare(0, 5, "data:") != 0)
            continue;

        std::string payload = line.substr(5);
        size_t p = payload.find_first_not_of(' ');
        if (p != std::string::npos)
            payload = payload.substr(p);
        if (payload.empty() || payload == "[DONE]")
            continue;

        Json::Value value;
        if (mcpParseJson(payload, value))
            handleMessage(value);
    }
}

void JZMcpClient::handleMessage(const Json::Value &msg)
{
    if (!msg.isObject())
        return;

    // 无 id 的是通知(如 notifications/...), 客户端目前不处理
    if (!msg.isMember("id") || !msg["id"].isInt())
        return;

    int id = msg["id"].asInt();
    if (msg.isMember("error"))
        finishPending(id, false, msg["error"]);
    else
        finishPending(id, true, msg.isMember("result") ? msg["result"] : Json::Value(Json::objectValue));
}

void JZMcpClient::finishPending(int id, bool ok, const Json::Value &value)
{
    auto it = m_pending.find(id);
    if (it == m_pending.end())
        return;

    ReplyCallback cb = it->second.cb;
    JZTimer *timer = it->second.timer;
    m_pending.erase(it);

    if (timer)
    {
        timer->stop();
        timer->deleteLater();
    }
    if (cb)
        cb(ok, value);
}

void JZMcpClient::clearPending(bool notify)
{
    std::map<int, Pending> pending;
    pending.swap(m_pending);
    if (!notify)
        return;

    Json::Value err = mcpMakeError(kClientError, m_error.empty() ? std::string("连接已关闭") : m_error);
    for (auto &kv : pending)
    {
        if (kv.second.timer)
        {
            kv.second.timer->stop();
            kv.second.timer->deleteLater();
        }
        if (kv.second.cb)
            kv.second.cb(false, err);
    }
}

void JZMcpClient::setFailed(const std::string &error)
{
    if (!m_error.empty())
        return;    // 已处于失败态(首个错误为准), 不重复通知
    m_error = error;
    m_ready = false;
    m_finished = true;
    clearPending(true);
    sigFinished.emit();     // 连接流程结束(失败), 原因见 errorString()
}

bool JZMcpClient::prepareConnect()
{
    close();
    m_error.clear();
    m_finished = false;

    // 两种传输都靠 JZApplication 的事件循环驱动(进程读管道 / HTTP)
    if (!jzApp)
    {
        m_error = "MCP 客户端依赖事件循环, 请先创建 JZApplication";
        return false;
    }
    return true;
}

// ==================== 异步建立连接 ====================
// 2026-07-28 无状态协议: 不做 initialize 握手, 只用 server/discover 探测服务器身份,
// 协议版本随每个请求的 _meta 携带。不支持需要握手的旧版服务器。
// 流程: beginNegotiate(server/discover) -> onDiscoverDone -> tools/list -> onToolsDone
//       -> 结束发 sigFinished(成功 isReady()==true; 任一步失败走 abortConnect)。
void JZMcpClient::beginNegotiate()
{
    sendRequest("server/discover", Json::Value(Json::objectValue),
                [this](bool ok, const Json::Value &value) { onDiscoverDone(ok, value); },
                m_timeoutMs);
}

void JZMcpClient::onDiscoverDone(bool ok, const Json::Value &value)
{
    if (!ok)
    {
        // 仅首次连接失败
        abortConnect("server/discover 失败: " + value.get("message", "").asString());
        return;
    }

    m_protocolVersion = value.get("protocolVersion", kProtocolVersion).asString();
    if (value.isMember("serverInfo"))
    {
        m_serverName = value["serverInfo"].get("name", "").asString();
        m_serverVersion = value["serverInfo"].get("version", "").asString();
    }

    sendRequest("tools/list", Json::Value(Json::objectValue),
                [this](bool o, const Json::Value &v) { onToolsDone(o, v); },
                m_timeoutMs);
}

void JZMcpClient::onToolsDone(bool ok, const Json::Value &value)
{
    if (!ok)
    {
        abortConnect("tools/list 失败: " + value.get("message", "").asString());
        return;
    }

    const Json::Value &tools = value["tools"];
    if (!tools.isArray())
    {
        abortConnect("tools/list 返回格式错误");
        return;
    }

    // 清掉上一批代理工具与名字映射
    for (auto *t : m_tools)
        delete t;
    m_tools.clear();
    m_remoteNames.clear();

    // 描述里标注来源, 便于大模型区分同名/近似的工具
    std::string source_tag = "[mcp:" + (m_serverName.empty() ? std::string("mcp") : m_serverName) + "] ";

    for (const auto &t : tools)
    {
        std::string name = t.get("name", "").asString();
        if (name.empty())
            continue;

        std::string local_name = makeLocalName(name);

        ToolDefine def;
        def.name = local_name;
        def.description = source_tag + t.get("description", "").asString();
        schemaToToolDefine(t.get("inputSchema", Json::Value(Json::objectValue)), def);

        JZMcpClientTool *tool = new JZMcpClientTool(this);
        tool->setParent(this);
        tool->setToolDefine(def);
        m_tools.push_back(tool);

        m_remoteNames[local_name] = name;
    }

    m_ready = true;
    m_finished = true;
    sigFinished.emit();
}

// 连接阶段失败统一收口: 首个错误经 setFailed 记录并发 sigFinished(重复失败不重复通知),
// 随后释放传输资源。
void JZMcpClient::abortConnect(const std::string &error)
{
    setFailed(error);
    close();
}

// ==================== 传输层事件 ====================
void JZMcpClient::onProcessOutput()
{
    if (!m_process)
        return;

    JZByteArray data = m_process->readAllStandardOutput();
    if (data.isEmpty())
        return;

    m_readBuffer.append(data.data(), data.size());

    size_t pos;
    while ((pos = m_readBuffer.find('\n')) != std::string::npos)
    {
        std::string line = m_readBuffer.substr(0, pos);
        m_readBuffer.erase(0, pos + 1);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!line.empty())
            onRecvText(line);
    }
}

void JZMcpClient::onHttpReply(JZNetworkReply *reply)
{
    reply->deleteLater();

    if (reply->error() != JZNetworkReply::NoError)
    {
        // HTTP 是无状态传输(每次请求独立), 没有"连接"可丢失, 故不重连, 直接永久失败
        setFailed("HTTP 请求失败: " + reply->errorString());
        return;
    }

    onRecvText(str_fromUtf8(reply->readAll()));
}

// ==================== tools/call ====================
void JZMcpClient::callTool(const std::string &name, const Json::Value &args, ReplyCallback cb)
{
    if (!m_ready)
    {
        if (cb)
            cb(false, mcpMakeError(kClientError, "MCP 连接不可用"));
        return;
    }

    Json::Value params(Json::objectValue);
    params["name"] = name;
    params["arguments"] = args.isNull() ? Json::Value(Json::objectValue) : args;

    sendRequest("tools/call", params, cb, m_callTimeoutMs, m_protocolVersion);
}
