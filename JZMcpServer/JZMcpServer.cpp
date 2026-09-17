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
        resp.setHeader("Access-Control-Allow-Methods", "POST, OPTIONS");
        resp.setHeader("Access-Control-Allow-Headers", "Content-Type, Accept");

        const std::string method = req.methodString();

        if (method == "OPTIONS")
        {
            resp.setStatusCode(204);
            resp.setBody(std::string());
            client->response(resp.toByteArray());
            return;
        }

        // 该端点仅接受 POST。流式走"POST 响应内联 SSE"(2026-07-28 标准):
        // 工具执行中先回 notifications/message(按 SSE 分块), 最后 tools/call 结果作为末块;
        // 本实现不提供独立的 GET/SSE 长连接。2025-06-18 客户端走 initialize 握手,
        // 握手后请求同样以 POST 提交(无 _meta), 由核心按版本构造响应。
        if (method != "POST")
        {
            resp.setStatusCode(405);
            resp.setHeader("Allow", "POST, OPTIONS");
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
