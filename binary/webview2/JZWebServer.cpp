#include "JZWebServer.h"
#include "JZApplication.h"

JZWebServer::JZWebServer()
    : m_static(jzApp->applicationDirPath() + "/app/webroot")
{
    std::string appDir = jzApp->applicationDirPath() + "/app";
    m_jsApp.init(appDir);

    // 动态 API 优先于静态兜底路由（同优先级按注册顺序，先注册者胜出）
    m_apiHandler.m_app = &m_jsApp;
    m_server.addRoute("/api/<path:rest>", &m_apiHandler);

    m_server.addRoute("/", &m_static);
    m_server.addRoute("/<path:path>", &m_static);
    m_server.listen(kPort);
}

JZWebServer::~JZWebServer()
{
    m_jsApp.shutdown();
    m_server.close();
}

std::string JZWebServer::baseUrl()
{
    return "http://127.0.0.1:" + std::to_string(kPort);
}

int JZWebServer::port()
{
    return kPort;
}
