#include "JZMcpServer.h"
#include "JZMcpCore.h"

#include "JZHttpServer.h"
#include "JZHttpHandler.h"
#include "JZHttpRequest.h"
#include "JZHttpRespone.h"
#include "JZByteArray.h"
#include "JZWebSocketServer.h"
#include "JZWebSocket.h"
#include "JZDebug.h"

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

        // 该端点仅接受 POST(本实现不提供 GET/SSE 长连接)
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

        // 异步处理: 结果通过回调回送, 不阻塞传输线程
        m_server->processText(bodyStr, [client, resp](bool hasResponse, const std::string& outJson) mutable {
            JZHttpRespone r = resp;
            if (hasResponse)
            {
                r.setStatusCode(200);
                r.setJson(outJson);
            }
            else
            {
                // 仅通知, 无响应体
                r.setStatusCode(202);
                r.setBody(std::string());
            }
            client->response(r.toByteArray());
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
    , m_ws(nullptr)
    , m_path("/mcp")
{
}

JZMcpServer::~JZMcpServer()
{
    close();
}

bool JZMcpServer::listenHttp(int port, const std::string& path)
{
    m_path = path.empty() ? std::string("/mcp") : path;

    if (!m_http)
        m_http = new JZHttpServer(this);

    if (!m_httpHandler)
        m_httpHandler = new JZMcpHttpHandler(this);

    m_http->addRoute(m_path, m_httpHandler);

    if (!m_http->listen(port))
    {
        jzError() << "JZMcpServer(HTTP) failed to listen on port" << port;
        return false;
    }

    jzInfo() << "JZMcpServer(HTTP) listening on http://0.0.0.0:" << port << m_path;
    return true;
}

bool JZMcpServer::listenWebSocket(int port)
{
    if (!m_ws)
        m_ws = new JZWebSocketServer(this);

    if (!m_ws->listen(port))
    {
        jzError() << "JZMcpServer(WebSocket) failed to listen on port" << port;
        return false;
    }

    connect(&m_ws->sigAccept, this, &JZMcpServer::onWsAccept);

    jzInfo() << "JZMcpServer(WebSocket) listening on ws://0.0.0.0:" << port;
    return true;
}

void JZMcpServer::onWsAccept()
{
    JZWebSocket* socket = m_ws->nextPendingConnection();
    if (!socket)
        return;

    connect(&socket->sigTextMessageReceived, this, &JZMcpServer::onWsMessage);
    connect(&socket->sigDisconnected, this, [socket]() { socket->deleteLater(); });
}

void JZMcpServer::onWsMessage(const std::string& text)
{
    // 信号发送者即对应的 WebSocket 连接
    auto* socket = jzobject_cast<JZWebSocket*>(sender());
    if (!socket)
        return;

    // WebSocket 为长连接, 每个文本帧都是一次请求, 必须回送响应
    processText(text, [socket](bool hasResponse, const std::string& outJson) {
        if (hasResponse && !outJson.empty())
            socket->sendTextMessage(outJson);
    });
}

bool JZMcpServer::processText(const std::string& in, ProcessCallback cb)
{
    return m_core->process(in, cb);
}

void JZMcpServer::close()
{
    if (m_http)
        m_http->close();
    if (m_ws)
        m_ws->close();
}

bool JZMcpServer::isListening() const
{
    if (m_http && m_http->isListening())
        return true;
    if (m_ws && m_ws->isListening())
        return true;
    return false;
}
