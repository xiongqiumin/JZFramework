#include <string>
#include "JZHttpServer.h"
#include "JZLog.h"
#include "JZApplication.h"
#include "JZDebug.h"
#include "JZStd.h"
#include "mongoose.h"

using namespace std;

// 判断 HTTP 响应头是否"自定界": 含 Content-Length 或 Transfer-Encoding: chunked 时为 true。
// 不自定界的响应(如 SSE 流式, 无 Content-Length)在 HTTP/1.1 keep-alive 下客户端无法判断
// 响应何时结束, 只能靠关闭连接来界定 —— 因此 sendFinished() 应自动关闭连接。
// 来源地址: 前面挂反向代理(nginx)时, 客户端 IP 在 X-Forwarded-For 里, 多级代理是逗号分隔的
// 列表, 取最左边一段(= 最初的客户端)。没有这个头就退回 TCP 对端地址。
static std::string clientAddrOf(const JZHttpRequest &request, const std::string &peerIp)
{
    std::string xff = request.header("X-Forwarded-For");
    if (!xff.empty())
    {
        size_t comma = xff.find(',');
        std::string first = str_trimmed((comma == std::string::npos) ? xff : xff.substr(0, comma));
        if (!first.empty())
            return first;
    }
    return peerIp;
}

static bool headIsDelimited(const std::string &head)
{
    // 只做 ASCII 小写: 头名本来就是 ASCII; 不能走 JZString(UTF-16) —— 一旦头里混进
    // 非 UTF-8 字节(压缩体/二进制 body)整串转换会失败返回空, 于是"有 Content-Length"
    // 被判成"不自定界", 响应后连接被关, 下一个 keep-alive 请求就 ConnectError。
    std::string h = head;
    for (size_t i = 0; i < h.size(); i++)
    {
        if (h[i] >= 'A' && h[i] <= 'Z')
            h[i] = (char)(h[i] - 'A' + 'a');
    }
    if (h.find("content-length:") != std::string::npos)
        return true;
    size_t te = h.find("transfer-encoding:");
    if (te != std::string::npos && h.find("chunked", te) != std::string::npos)
        return true;
    return false;
}

JZHttpServerClient::JZHttpServerClient(JZObject* parent) 
    : JZNetObject(parent)
{
    m_conn = INVALID_NET_HANDLE;
    m_responseStarted = false;
    m_responseDelimited = true;
}

JZHttpServerClient::~JZHttpServerClient()
{
    closeConn(false);
}

void JZHttpServerClient::onNetEvent(JZNetResEvent* event)
{
    JZHttpServer* server = jzobject_cast<JZHttpServer*>(parent());
    if (!server) 
        return;

    // 流式响应：上一次写入已落 socket，回调 handler 继续写下一块
    if (m_handle)
        m_handle->onNetEvent(this, event);

    if (event->event == JZ_NET_EV_HTTP_MSG)
    {
        server->onHttpMessage(this, event->binData);
    }
    else if (event->event == JZ_NET_EV_WRITE)
    {        
    }
    else if (event->event == JZ_NET_EV_CLOSE)
    {
        m_handle = nullptr;
        deleteLater();
    }
}

void JZHttpServerClient::setHandle(JZHttpHandler* handle)
{
    m_handle = handle;
}

void JZHttpServerClient::sendFinished()
{
    JZNetReqEvent* req = new JZNetReqEvent();
    req->netCmd = JZNetReqEvent::Net_HttpReplyFinish;
    req->conn = m_conn;
    postReqEvent(req);

    // 连接是否关闭:
    //  - 请求未要求 keep-alive, 或
    //  - 响应不自定界(无 Content-Length 也无 chunked, 如 SSE 流式)→ 只能靠关闭连接界定响应结束
    // 否则(有 Content-Length 的普通响应)保持 keep-alive 复用连接。无需业务代码手动干预。
    if (!m_keepAlive || !m_responseDelimited)
        closeConn(false);
}

void JZHttpServerClient::response(const JZByteArray& buffer)
{
    write(buffer.data(), buffer.size(), true);        
}

void JZHttpServerClient::response(const JZHttpRespone& resp, const JZHttpRequest& req)
{
    JZHttpRespone r(resp);
    r.applyContentEncoding(req.header("Accept-Encoding"));
    response(r.toByteArray());
}

void JZHttpServerClient::write(const JZByteArray& data, bool is_finish)
{
    write(data.data(), data.size(), is_finish);    
}

void JZHttpServerClient::write(const char *data, int size, bool is_finish)
{
    // 首次写入时解析响应头, 判定响应是否自定界(决定是否在 sendFinished 时关闭连接)。
    // 头通常随首段写入一次性到达(普通响应头+体合一, SSE 响应头单独一段); 若被拆分则累积到收全再判定。
    if (!m_responseStarted)
    {
        m_headAccum.append(data, size);
        auto pos = m_headAccum.find("\r\n\r\n");
        if (pos != std::string::npos)
        {
            // 只把头(到 \r\n\r\n 为止)拿去判定: m_headAccum 累积的是整块数据,
            // 后面可能紧跟二进制 body(压缩响应), 整块一起转小写会失败
            m_responseDelimited = headIsDelimited(m_headAccum.substr(0, pos + 4));
            m_responseStarted = true;
            m_headAccum.clear();
        }
    }

    writeConn(WriteBin, data, size);
    if (is_finish)
    {
        sendFinished();
    }
}

//JZHttpServer
JZHttpServer::JZHttpServer(JZObject* parent)
    :JZNetObject(parent)
{   
    m_conn = INVALID_NET_HANDLE;        
}

JZHttpServer::~JZHttpServer()
{    
    close();    
}

bool JZHttpServer::isListening() const
{
    // m_conn 为 uint64 连接句柄，显式转为 bool 语义，避免误用其数值
    return m_conn != INVALID_NET_HANDLE;
}

bool JZHttpServer::listen(int port)
{    
    return listen("0.0.0.0", port);
}

bool JZHttpServer::listen(std::string ip, int port)
{    
    char ip_addr[32];
    sprintf(ip_addr, "%s:%d", ip.c_str(), port);    

    JZSocketInfo info;
    info.url = ip_addr;
    info.type = JZSocketInfo::HttpServer;
    m_conn = openConn(info);
    if (!m_conn)
        return false;                    
    
    return true;
}

void JZHttpServer::close()
{
    if (m_conn)
    {
        closeConn(false);
        m_conn = INVALID_NET_HANDLE;
    }
}

void JZHttpServer::addRoute(const std::string& path, JZHttpHandler* handler)
{
    JZ_ASSERT(!handler->parent() || handler->parent() == this);
    handler->setParent(this);
    m_route.addRoute(path, handler);
}

JZNetObject* JZHttpServer::onAccept(uint64_t conn, const JZNetAddr &addr)
{
    JZHttpServerClient* client = new JZHttpServerClient(this);
    client->m_conn = conn;
    client->m_connType = JZSocketInfo::HttpClientServer;
    client->moveToThread(this->thread());
    client->setConnAddr(addr);      // 请求对象/JS 侧 req.remoteAddress 都靠它
    return client;
}

void JZHttpServer::onHttpMessage(JZHttpServerClient* client, const JZByteArray& buffer)
{
    struct mg_http_message hm;
    int parse_result = mg_http_parse((const char*)buffer.data(), buffer.size(), &hm);

    // 提取客户端支持的压缩编码，供错误响应压缩使用
    std::string accept_encoding;
    struct mg_str* ae = mg_http_get_header(&hm, "Accept-Encoding");
    if (ae)
        accept_encoding = std::string(ae->buf, ae->len);
    
    if (parse_result <= 0)
    {
        JZHttpRespone response;
        response.setStatusCode(400);
        response.setText("Bad Request");
        response.applyContentEncoding(accept_encoding);
        client->response(response.toByteArray());
        return;
    }

    // 判断 keep-alive: HTTP/1.1 默认 keep-alive，除非 Connection: close
    client->m_keepAlive = true;
    struct mg_str *conn_hdr = mg_http_get_header(&hm, "Connection");
    if (conn_hdr)
    {
        std::string conn_val(conn_hdr->buf, conn_hdr->len);
        if (conn_val == "close" || conn_val == "Close")
            client->m_keepAlive = false;
    }

    // 每个请求(keep-alive 会复用同一 client 对象)重置响应自定界判定状态
    client->m_responseStarted = false;
    client->m_responseDelimited = true;
    client->m_headAccum.clear();

    JZHttpRequest request;
    
    std::string method(hm.method.buf, hm.method.len);
    request.setMethodString(method);
    
    std::string uri(hm.uri.buf, hm.uri.len);
    request.setPath(uri);

    std::string query(hm.query.buf, hm.query.len);
    request.setQuery(query);   // 内部自动解析并填充 queryParam() / queryParams()
    
    for (int i = 0; i < MG_MAX_HTTP_HEADERS; i++)
    {
        if (hm.headers[i].name.buf != nullptr && hm.headers[i].name.len > 0)
        {
            std::string name(hm.headers[i].name.buf, hm.headers[i].name.len);
            std::string value(hm.headers[i].value.buf, hm.headers[i].value.len);
            request.setHeader(name, value);
        }
    }
    
    if (hm.body.buf != nullptr && hm.body.len > 0)
    {
        JZByteArray body(hm.body.buf, hm.body.len);
        request.setBody(body);
    }

    // 来源地址(直连 = TCP 对端; 经 nginx 等反向代理 = X-Forwarded-For 第一段)
    request.setRemoteAddr(clientAddrOf(request, client->peerIp()));

    JZHttpHandler* handler = nullptr;
    std::map<std::string, std::string> params;
    
    if (m_route.match(uri, handler, params))
    {
        for (const auto& param : params)
        {
            request.setParam(param.first, param.second);
        }
        
        handler->handle(client, request);
    }
    else
    {
        JZHttpRespone response;
        response.setStatusCode(404);
        response.setText("Not Found");
        response.applyContentEncoding(request.header("Accept-Encoding"));
        client->response(response.toByteArray());
    }
}

void JZHttpServer::onNetEvent(JZNetResEvent* event)
{

}