#include "JZApplication.h"
#include "JZWebSocketServer.h"
#include "JZDebug.h"

//JZWebSocketServer
JZ_OBJECT_IMPL(JZWebSocketServer, JZNetObject)

JZWebSocketServer::JZWebSocketServer(JZObject *parent)
    :JZNetObject(parent), sigAccept(this)
{
    m_listen = INVALID_NET_HANDLE;
}

JZWebSocketServer::~JZWebSocketServer()
{
    close();
}

void JZWebSocketServer::close()
{
    if (m_listen != INVALID_NET_HANDLE)
    {
        closeConn(m_listen, true);
        m_listen = INVALID_NET_HANDLE;
    }
}

bool JZWebSocketServer::isListening() const
{
    return (m_listen != INVALID_NET_HANDLE);
}

bool JZWebSocketServer::listen(int port)
{
    char ip_addr[32];
    sprintf(ip_addr, "0.0.0.0:%d", port);

    JZSocketInfo info;
    info.url = ip_addr;
    info.type = JZSocketInfo::WebSocketListen;
    m_listen = openConn(info);
    if (!m_listen)
        return false;

    return true;
}

JZNetObject* JZWebSocketServer::onAccept(uint64_t conn)
{
    JZWebSocket* socket = new JZWebSocket(this);
    socket->m_conn = conn;
    socket->m_state = ConnectedState;
    socket->moveToThread(this->thread());

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