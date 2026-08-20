#include <string>
#include "JZNetServer.h"
#include "JZLog.h"
#include "JZApplication.h"
#include "JZPlatform.h"
#include "JZNetClient.h"
#include "JZNetPacket.h"
#include "JZDebug.h"

using namespace std;

JZNetServer::JZNetServer(JZObject *parent)
    : JZNetObject(parent), sigAccept(this), sigDisconnect(this), sigPacketReceive(this)
{   
    m_conn = INVALID_NET_HANDLE;        

    m_timer = new JZTimer(this);
    m_timer->setSingleShot(false);
    connect(&m_timer->sigTimeout, this, &JZNetServer::onHandTimer);
    m_timer->start(1000);
}

JZNetServer::~JZNetServer()
{    
    if (m_timer)
    {
        m_timer->stop();
        delete m_timer;
    }
    close();    
}

void JZNetServer::setToken(std::string token)
{
    m_token = token;
}

std::string JZNetServer::token()
{
    return m_token;
}

bool JZNetServer::isListening() const
{
    return m_conn != INVALID_NET_HANDLE;
}

bool JZNetServer::listen(int port)
{    
    return listen("0.0.0.0", port);
}

bool JZNetServer::listen(std::string ip, int port)
{    
    char ip_addr[32];
    sprintf(ip_addr, "%s:%d", ip.c_str(), port);    

    JZSocketInfo info;
    info.url = ip_addr;
    info.type = JZSocketInfo::TcpServer;
    if (!openConn(info))
        return false;                    
    
    return true;
}


void JZNetServer::close()
{
    if (m_conn)
    {
        closeConn(false);
        m_conn = INVALID_NET_HANDLE;
    }
}

void JZNetServer::sendPacket(uint64_t c, const JZNetPack *pack)
{
    auto it = m_clients.find(c);
    JZ_ASSERT(it != m_clients.end());

    it->second->sendPacket(pack);
}

void JZNetServer::sendPacketToAll(const JZNetPack *pack)
{
    for (auto &it : m_clients)
    {
        it.second->sendPacket(pack);
    }    
}

void JZNetServer::sendPacketToAllExclude(uint64_t c, const JZNetPack *pack)
{
    for (auto &it : m_clients)
    {
        if(it.first != c)
            it.second->sendPacket(pack);    
    }    
}

void JZNetServer::closeClient(uint64_t c)
{
    auto it = m_clients.find(c);
    JZ_ASSERT(it != m_clients.end());
    
    it->second->disconnectFromHost();
}

void JZNetServer::onClientDisconnect()
{
    JZNetClient *socket = (JZNetClient *)sender();
    socket->deleteLater();
    
    auto id = socket->m_conn;
    auto it = m_clients.find(id);
    if (it == m_clients.end())
        return;

    removeHandInfo(id);
    m_clients.erase(id);
    sigDisconnect.emit(id);
}

void JZNetServer::onClientPackReceive(JZNetPackPtr ptr)
{
    JZNetClient *socket = (JZNetClient *)sender();
    auto id = socket->m_conn;
    auto it = m_clients.find(id);
    if (it == m_clients.end())
        return;

    sigPacketReceive.emit(id, ptr);
}

uint64_t JZNetServer::nextPendingConnection()
{
    if (m_pendingConnections.empty())
        return 0;

    JZNetClient* client = m_pendingConnections.front();
    m_pendingConnections.pop_front();
    return client->m_conn;
}

JZNetObject *JZNetServer::onAccept(uint64_t conn)
{
    JZNetClient *socket = new JZNetClient(this);
    socket->m_conn = conn;
    socket->m_state = ConnectingState;
    socket->moveToThread(this->thread());
    socket->m_server = this;

    connect(&socket->sigDisconnect, this, &JZNetServer::onClientDisconnect);
    connect(&socket->sigPacketReceive, this, &JZNetServer::onClientPackReceive);

    JZObjectPointer socket_ptr = socket->guardPointer();
    auto notify = [this, conn, socket_ptr]
    {
        if (socket_ptr)
        {
            JZNetClient *socket = (JZNetClient *)socket_ptr.object();
            m_clients[conn] = socket;
            // 握手由客户端发起(发送 Hand),服务器收到后回 HandRes,
            // 连接就绪在握手完成时通过 sigAccept 通知,这里不再入队

            // 记录握手起始时间,由服务器的 1 秒定时器统一判断超时
            m_handTime[conn] = JZPlatform::getTickCountMs();
        }
    };

    runOnObject(this, notify);
    return socket;
}

void JZNetServer::removeHandInfo(uint64_t conn)
{
    auto it = m_handTime.find(conn);
    if (it != m_handTime.end())
        m_handTime.erase(it);
}

void JZNetServer::onHandTimer()
{
    uint64_t now = JZPlatform::getTickCountMs();
    for (auto it = m_handTime.begin(); it != m_handTime.end(); )
    {
        uint64_t conn = it->first;
        auto cit = m_clients.find(conn);
        if (cit == m_clients.end() || cit->second->state() != ConnectingState)
        {
            // 连接已断开或握手已完成,清理记录
            it = m_handTime.erase(it);
            continue;
        }

        if (now - it->second >= 30000)
        {
            jzWarning() << "handshake timeout, close connection" << conn;
            cit->second->closeInternal(true);
            it = m_handTime.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void JZNetServer::onNetEvent(JZNetResEvent*event)
{
}