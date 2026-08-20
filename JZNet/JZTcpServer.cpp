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
    m_conn = INVALID_NET_HANDLE;        
}

JZTcpServer::~JZTcpServer()
{    
    close();    
}

bool JZTcpServer::isListening() const
{
    return m_conn != 0;
}

int JZTcpServer::port() const
{
    return m_port;
}

bool JZTcpServer::listen(int port)
{    
    return listen("0.0.0.0", port);
}

bool JZTcpServer::listen(std::string ip, int port)
{    
    m_port = port;

    char ip_addr[32];
    sprintf(ip_addr, "%s:%d", ip.c_str(), port);    

    JZSocketInfo info;
    info.url = ip_addr;
    info.type = JZSocketInfo::TcpServer;
    if (!openConn(info))
        return false;                    
    
    return true;
}

void JZTcpServer::close()
{
    if (m_conn != INVALID_NET_HANDLE)
    {
        closeConn(false);
        m_port = 0;
        m_conn = INVALID_NET_HANDLE;
    }
}

JZNetObject* JZTcpServer::onAccept(uint64_t conn)
{
    JZTcpSocket* socket = new JZTcpSocket(this);
    socket->m_conn = conn;
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

    runOnObject(this, notify);
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