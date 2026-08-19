#include <string>
#include "JZHttpServer.h"
#include "JZLog.h"
#include "JZApplication.h"
#include "JZDebug.h"
#include "mongoose.h"

using namespace std;

JZHttpServerClient::JZHttpServerClient(JZObject* parent) 
    : JZNetObject(parent)
{
    m_conn = INVALID_NET_HANDLE;
}

JZHttpServerClient::~JZHttpServerClient()
{
    closeConn(m_conn, false);
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

    // 非 keep-alive: 通知 mongoose 响应完成后关闭连接
    if (!m_keepAlive)
        closeConn(m_conn, false);
}

void JZHttpServerClient::response(const JZByteArray& buffer)
{
    write(buffer.data(), buffer.size(), true);        
}

void JZHttpServerClient::write(const JZByteArray& data, bool is_finish)
{
    write(data.data(), data.size(), is_finish);    
}

void JZHttpServerClient::write(const char *data, int size, bool is_finish)
{
    writeConn(m_conn, WriteBin, data, size);
    if (is_finish)
    {
        sendFinished();
    }
}

//JZHttpServer
JZHttpServer::JZHttpServer(JZObject* parent)
    :JZNetObject(parent)
{   
    m_listen = INVALID_NET_HANDLE;        
}

JZHttpServer::~JZHttpServer()
{    
    close();    
}

bool JZHttpServer::isListening() const
{
    return m_listen;
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
    info.sslCert = m_sslCert;
    info.sslKey = m_sslKey;
    info.sslCa = m_sslCa;
    m_listen = openConn(info);
    if (!m_listen)
        return false;                    
    
    return true;
}

void JZHttpServer::close()
{
    if (m_listen)
    {
        closeConn(m_listen, false);
        m_listen = INVALID_NET_HANDLE;
    }
}

void JZHttpServer::addRoute(const std::string& path, JZHttpHandler* handler)
{
    handler->setParent(this);
    m_route.addRoute(path, handler);
}

JZNetObject* JZHttpServer::onAccept(uint64_t conn)
{
    JZHttpServerClient* client = new JZHttpServerClient(this);
    client->m_conn = conn;
    client->moveToThread(this->thread());
    return client;
}

void JZHttpServer::onHttpMessage(JZHttpServerClient* client, const JZByteArray& buffer)
{
    struct mg_http_message hm;
    int parse_result = mg_http_parse((const char*)buffer.data(), buffer.size(), &hm);
    
    if (parse_result <= 0)
    {
        JZHttpRespone response;
        response.setStatusCode(400);
        response.setText("Bad Request");
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

    JZHttpRequest request;
    
    std::string method(hm.method.buf, hm.method.len);
    request.setMethodString(method);
    
    std::string uri(hm.uri.buf, hm.uri.len);
    request.setPath(uri);

    std::string query(hm.query.buf, hm.query.len);
    request.setQuery(query);
    
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
        client->response(response.toByteArray());
    }
}

void JZHttpServer::onNetEvent(JZNetResEvent* event)
{

}