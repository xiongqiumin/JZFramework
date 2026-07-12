#include <string>
#include "JZTcpServer.h"
#include "JZLog.h"
#include "JZApplication.h"
#include "JZDebug.h"

using namespace std;

JZTcpServer::JZTcpServer(JZObject *parent)
    :JZNetObject(parent), sigAccept(this)
{
    m_port = 0;
    m_listen = INVALID_NET_HANDLE;        
}

JZTcpServer::~JZTcpServer()
{    
    close();    
}

bool JZTcpServer::isListening() const
{
    return m_listen != 0;
}

int JZTcpServer::port() const
{
    return m_port;
}

bool JZTcpServer::listen(int port)
{    
    m_port = port;

    char ip_addr[32];
    sprintf(ip_addr, "0.0.0.0:%d", port);    

    JZSocketInfo info;
    info.url = ip_addr;
    info.type = JZSocketInfo::TcpServer;
    m_listen = openConn(info);
    if (!m_listen)
        return false;                    
    
    return true;
}

void JZTcpServer::close()
{
    if (m_listen != INVALID_NET_HANDLE)
    {
        closeConn(m_listen, false);
        m_port = 0;
        m_listen = INVALID_NET_HANDLE;
    }
}

JZNetObject* JZTcpServer::onAccept(uint64_t conn)
{
    JZTcpSocket* socket = new JZTcpSocket(this);
    socket->m_client = conn;
    socket->m_state = ConnectedState;
    socket->moveToThread(this->thread());

    JZObjectPointer socket_ptr = socket->guardPointer();
    auto notify = [this, socket_ptr]
    {
        if (socket_ptr)
        {
            JZTcpSocket* socket = (JZTcpSocket*)socket_ptr.object();
            m_pendingConnections.push_back(socket);
            this->sigAccept.emit();
        }
    };

    runOnThread(this->thread(), this, notify);
    return socket;
}

JZTcpSocket* JZTcpServer::nextPendingConnection()
{
    if (m_pendingConnections.empty())
        return nullptr;

    JZTcpSocket* socket = m_pendingConnections.front();
    m_pendingConnections.pop_front();
    return socket;
}

void JZTcpServer::onNetEvent(JZNetResEvent*event)
{
}