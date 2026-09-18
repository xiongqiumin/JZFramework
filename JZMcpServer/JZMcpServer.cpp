#include "JZMcpServer.h"
#include "JZMcpCore.h"

#include "JZHttpServer.h"
#include "JZHttpHandler.h"
#include "JZHttpRequest.h"
#include "JZHttpRespone.h"
#include "JZByteArray.h"
#include "JZDebug.h"

#include <memory>

// ======================================================================
// HTTP 处理器: 把 POST /mcp 的 body 交给 JZMcpCore 处理
// ======================================================================

class JZMcpHttpHandler : public JZHttpHandler
{
public:
    explicit JZMcpHttpHandler(JZMcpServer* server) : m_server(server) {}

    void handle(JZHttpServerClient* client, const JZHttpRequest& req) override
    {
        JZHttpRespone resp;

        // 允许简单的跨域访问(方便浏览器端 MCP 客户端调试)
        resp.setHeader("Access-Control-Allow-Origin", "*");
        resp.setHeader("Access-Control-Allow-Methods", "POST, GET, OPTIONS");
        resp.setHeader("Access-Control-Allow-Headers", "Content-Type, Accept, Mcp-Session-Id");

        const std::string method = req.methodString();

        if (method == "OPTIONS")
        {
            resp.setStatusCode(204);
            resp.setBody(std::string());
            client->response(resp.toByteArray());
            return;
        }

        // 该端点接受 POST 与 GET:
        //   - POST: 提交 JSON-RPC 请求, 流式走"POST 响应内联 SSE"(2026-07-28 标准)——
        //     工具执行中先回 notifications/message(按 SSE 分块), 最后 tools/call 结果作为末块;
        //   - GET: 建立服务端→客户端的 SSE 推送长连接(见下方分支), 用于服务器主动通知
        //     (logging / tools/list_changed 等), 经 broadcastNotification()/logMessage() 推送。
        // 2025-06-18 客户端走 initialize 握手, 握手后请求同样以 POST 提交(无 _meta),
        // 由核心按版本构造响应。
        if (method == "GET")
        {
            // MCP Streamable HTTP: GET 用于建立服务端→客户端的 SSE 推送流(长连接)。
            // 客户端必须以 Accept: text/event-stream 请求, 否则按规范返回 406(Not Acceptable)。
            const std::string accept = req.header("Accept");
            if (accept.find("text/event-stream") == std::string::npos)
            {
                JZHttpRespone r = resp;
                r.setStatusCode(406);
                r.setHeader("Content-Type", "application/json");
                r.setJson(R"({"jsonrpc":"2.0","error":{"code":-32000,"message":"Not Acceptable: GET /mcp requires Accept: text/event-stream"},"id":null})");
                client->response(r.toByteArray());
                return;
            }

            // 打开 SSE 流: 无 Content-Length, 首块发一个注释帧让客户端确认流已建立;
            // 连接保持打开(不调用 sendFinished), 直到客户端断开 —— 期间服务端可经
            // broadcastNotification()/logMessage() 主动推送通知。注册为推送目标,
            // 连接断开由 sigDestroyed 自动移除。
            JZHttpRespone head = resp;
            head.setStatusCode(200);
            head.setHeader("Content-Type", "text/event-stream");
            head.setHeader("Cache-Control", "no-cache");
            head.setHeader("X-Accel-Buffering", "no");   // 关掉反代缓冲
            client->write(head.toHeadByteArray(0, false), false);
            const std::string sse_hello = ": connected\n\n";
            client->write(sse_hello.data(), (int)sse_hello.size(), false);

            m_server->addSseClient(client);
            connect(&client->sigDestroyed, [this, client](JZObject*) {
                m_server->removeSseClient(client);
            });
            return;
        }

        if (method != "POST")
        {
            resp.setStatusCode(405);
            resp.setHeader("Allow", "POST, GET, OPTIONS");
            resp.setText("Method Not Allowed");
            client->response(resp.toByteArray());
            return;
        }

        const JZByteArray& body = req.body();
        std::string bodyStr(body.data(), body.size());

        // 异步处理: 结果通过回调回送, 不阻塞传输线程。
        // 回调可调用多次: 首个分块决定用 SSE 还是普通 JSON; finished=true 结束响应。
        // sentHeaders 用 shared_ptr<bool> 共享: onToolNotify 每次会拷贝 cb(std::function 复制闭包),
        // 若按值捕获 bool, 拷贝版与原版各自一份状态, 结束时原版仍认为"未发头"会再发一遍完整
        // HTTP 头 → 响应里出现两个 HTTP 头, 连接不关闭, 客户端一直等到超时。共享同一份状态即可。
        m_server->processText(bodyStr, [client, resp, sentHeaders = std::make_shared<bool>(false)]
                               (bool hasResponse, const std::string& outJson, bool finished) mutable {
            const auto sseFrame = [](const std::string& json) {
                return std::string("data: ") + json + "\n\n";
            };

            // 纯通知 / 被取消的请求(无响应体): 尚未发头就回 202; 已经开始 SSE 流的话
            // 只能把流结束掉(关闭连接), 否则对端这个 POST 会一直挂着等"结束"。
            if (!hasResponse)
            {
                if (*sentHeaders)
                {
                    client->write("", 0, true);
                    return;
                }
                JZHttpRespone r = resp;
                r.setStatusCode(202);
                r.setBody(std::string());
                client->response(r.toByteArray());
                return;
            }

            if (!*sentHeaders)
            {
                *sentHeaders = true;
                if (finished)
                {
                    // 普通单次 JSON 响应
                    JZHttpRespone r = resp;
                    r.setStatusCode(200);
                    r.setJson(outJson);
                    client->response(r.toByteArray());
                }
                else
                {
                    // 开始 SSE 流: 无 Content-Length 头部 + 首块
                    // 连接关闭由 JZHttpServerClient 自动判定: 响应无 Content-Length(不自定界),
                    // 终帧 sendFinished() 会关闭连接, 客户端据此得知响应结束。
                    JZHttpRespone head = resp;
                    head.setStatusCode(200);
                    head.setHeader("Content-Type", "text/event-stream");
                    head.setHeader("Cache-Control", "no-cache");
                    head.setHeader("Connection", "close");
                    head.setHeader("X-Accel-Buffering", "no");   // 关掉反代缓冲
                    client->write(head.toHeadByteArray(0, false), false);
                    std::string frame = sseFrame(outJson);
                    client->write(frame.data(), (int)frame.size(), false);
                }
            }
            else
            {
                std::string frame = sseFrame(outJson);
                client->write(frame.data(), (int)frame.size(), finished);
            }
        });
    }

private:
    JZMcpServer* m_server;
};

// ======================================================================
// JZMcpServer
// ======================================================================
JZMcpServer::JZMcpServer(JZObject* parent)
    : JZObject(parent)
    , m_core(new JZMcpCore(this))
    , m_http(nullptr)
    , m_httpHandler(nullptr)
    , m_path("/mcp")
{
}

JZMcpServer::~JZMcpServer()
{
    close();
}

bool JZMcpServer::listenHttp(int port, const std::string& path)
{
    return listenHttp("0.0.0.0", port, path);
}

bool JZMcpServer::listenHttp(std::string ip, int port, const std::string& path)
{
    m_path = path.empty() ? std::string("/mcp") : path;

    if (!m_http)
        m_http = new JZHttpServer(this);

    if (!m_httpHandler)
        m_httpHandler = new JZMcpHttpHandler(this);

    m_http->addRoute(m_path, m_httpHandler);

    if (!m_http->listen(ip, port))
    {
        jzError() << "JZMcpServer(HTTP) failed to listen on port" << port;
        return false;
    }

    jzInfo().nospace() << "JZMcpServer(HTTP) listening on http://" + ip + ":" + std::to_string(port);
    return true;
}

bool JZMcpServer::processText(const std::string& in, ProcessCallback cb)
{
    return m_core->process(in, cb);
}

// ======================================================================
// SSE 推送流(GET /mcp 建立的服务器→客户端长连接)
// ======================================================================
void JZMcpServer::addSseClient(JZHttpServerClient* client)
{
    if (!client)
        return;
    std::lock_guard<std::mutex> lk(m_sseMutex);
    m_sseClients.push_back({client->guardPointer(), client});
}

void JZMcpServer::removeSseClient(JZHttpServerClient* client)
{
    std::lock_guard<std::mutex> lk(m_sseMutex);
    for (auto it = m_sseClients.begin(); it != m_sseClients.end();)
    {
        if (it->client == client)
            it = m_sseClients.erase(it);
        else
            ++it;
    }
}

bool JZMcpServer::hasSseClients() const
{
    std::lock_guard<std::mutex> lk(m_sseMutex);
    return !m_sseClients.empty();
}

void JZMcpServer::broadcastNotification(const std::string& jsonRpcNotification)
{
    if (jsonRpcNotification.empty())
        return;

    // SSE 帧: "data: <json>\n\n"
    const std::string frame = std::string("data: ") + jsonRpcNotification + "\n\n";

    // 拷贝快照后解锁再写: 避免在持有锁时回调到网络层; guard.object() 为空表示该连接
    // 已断开(对象析构中), 跳过即可, 不存在悬空写入。
    std::vector<SseClient> snapshot;
    {
        std::lock_guard<std::mutex> lk(m_sseMutex);
        snapshot = m_sseClients;
    }
    for (const auto& e : snapshot)
    {
        if (e.guard.object())
            e.client->write(frame.data(), (int)frame.size(), false);
    }
}

void JZMcpServer::logMessage(const std::string& level, const std::string& message)
{
    // 构造 notifications/message(JSON-RPC 通知, 无 id)
    Json::Value params(Json::objectValue);
    params["level"] = level.empty() ? "info" : level;   // "debug" | "info" | "warning" | "error"
    params["logger"] = "JZMcpServer";
    params["data"] = message;

    Json::Value note(Json::objectValue);
    note["jsonrpc"] = "2.0";
    note["method"] = "notifications/message";
    note["params"] = params;

    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    builder["emitUTF8"] = true;
    broadcastNotification(Json::writeString(builder, note));
}

void JZMcpServer::close()
{
    if (m_http)
        m_http->close();
}

bool JZMcpServer::isListening() const
{
    if (m_http && m_http->isListening())
        return true;
    return false;
}
