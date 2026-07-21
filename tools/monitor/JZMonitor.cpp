#include "JZMonitor.h"
#include "JZTcpSocket.h"
#include "JZDebug.h"
#include "JZSignal.h"

JZ_OBJECT_IMPL(JZMonitor, JZObject)

JZMonitor::JZMonitor(JZObject *parent) : JZObject(parent)
{
}

JZMonitor::~JZMonitor()
{
    stop();
}

bool JZMonitor::start(int webPort, int monitorPort)
{
    m_http = new JZHttpServer(this);
    m_tcp  = new JZTcpServer(this);

    // TODO: 注册 HTTP 路由 (JZHttpServer::addRoute)
    // TODO: 处理 Web 请求 (JZHttpHandler)

    // 监控连接接入
    connect(&m_tcp->sigAccept, this, &JZMonitor::onTcpAccept);

    bool ok = true;
    if (!m_http->listen(webPort)) {
        jzWarning() << "HttpServer failed to listen on port" << webPort;
        ok = false;
    }
    if (!m_tcp->listen(monitorPort)) {
        jzWarning() << "TcpServer failed to listen on port" << monitorPort;
        ok = false;
    }
    return ok;
}

void JZMonitor::stop()
{
    if (m_http) { m_http->close(); delete m_http; m_http = nullptr; }
    if (m_tcp)  { m_tcp->close();  delete m_tcp;  m_tcp  = nullptr; }
}

void JZMonitor::onTcpAccept()
{
    JZTcpSocket *sock = m_tcp->nextPendingConnection();
    if (!sock) return;

    // TODO: 处理监控连接 (读取数据 / 维护会话)
}
