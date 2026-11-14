#include "JZApplication.h"
#include "JZWebSocketServer.h"
#include "JZDebug.h"

//JZWebSocketServer
JZ_OBJECT_IMPL(JZWebSocketServer, JZNetObject)

JZWebSocketServer::JZWebSocketServer(JZObject *parent)
    :JZNetObject(parent), sigAccept(this)
{
    m_conn = INVALID_NET_HANDLE;
}

JZWebSocketServer::~JZWebSocketServer()
{
    close();
}

void JZWebSocketServer::close()
{
    if (m_conn != INVALID_NET_HANDLE)
    {
        closeConn(true);
        m_conn = INVALID_NET_HANDLE;
    }
}

bool JZWebSocketServer::isListening() const
{
    return (m_conn != INVALID_NET_HANDLE);
}

bool JZWebSocketServer::listen(int port)
{
    return listen("0.0.0.0", port);
}

bool JZWebSocketServer::listen(std::string ip, int port)
{
    char ip_addr[32];
    sprintf(ip_addr, "%s:%d", ip.c_str(), port);

    JZSocketInfo info;
    info.url = ip_addr;
    info.type = JZSocketInfo::WebSocketListen;
    if (!openConn(info))
        return false;

    return true;
}

JZNetObject* JZWebSocketServer::onAccept(uint64_t conn, const JZNetAddr &addr)
{
    JZWebSocket* socket = new JZWebSocket(this);
    socket->m_conn = conn;
    socket->m_connType = JZSocketInfo::WebSocketServer;
    socket->m_state = ConnectedState;
    socket->moveToThread(this->thread());
    socket->setConnAddr(addr);      // 先写地址: 下面 sigAccept 的回调里就要能读到

    JZObjectPointer socket_ptr = socket->guardPointer();
    auto notify = [this, socket_ptr]
    {
        if (socket_ptr)
        {
            JZWebSocket* socket = (JZWebSocket*)socket_ptr.object();
            m_pendingConnections.push_back(socket);
            this->sigAccept.emit();
        }
    };

    runOnObject(this, notify);
    return socket;
}

JZWebSocket* JZWebSocketServer::nextPendingConnection()
{
    if (m_pendingConnections.empty())
        return nullptr;

    JZWebSocket* socket = m_pendingConnections.front();
    m_pendingConnections.pop_front();
    return socket;
}

void JZWebSocketServer::onNetEvent(JZNetResEvent *event)
{
}