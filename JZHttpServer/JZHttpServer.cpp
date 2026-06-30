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
    JZHttpServer* server = (JZHttpServer*)(parent());
    if (event->event == JZ_NET_EV_HTTP_MSG)
    {
        server->onHttpMessage(this, event->binData);
    }
}

void JZHttpServerClient::response(const JZByteArray& buffer)
{
    writeConn(m_conn, WriteBin, buffer.data(), buffer.size());
    deleteLater();
}

void JZHttpServerClient::write(const JZByteArray& data, bool is_finish)
{
    writeConn(m_conn, WriteBin, data.data(), data.size());
    if(is_finish)
        deleteLater();
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
    char ip_addr[32];
    sprintf(ip_addr, "0.0.0.0:%d", port);    

    JZSocketInfo info;
    info.url = ip_addr;
    info.type = JZSocketInfo::HttpServer;
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
    m_route.addRoute(path, handler);
}

JZNetObject* JZHttpServer::onAccept(uint64_t conn)
{
    JZHttpServerClient* client = new JZHttpServerClient(this);
    client->m_conn = conn;
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

    JZHttpRequest request;
    
    std::string method(hm.method.buf, hm.method.len);
    request.setMethodString(method);
    
    std::string uri(hm.uri.buf, hm.uri.len);
    request.setPath(uri);
    
    jzDebug() << "onHttpMessage" << uri;

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