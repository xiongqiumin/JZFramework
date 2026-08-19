#include <string>
#include "JZNetServer.h"
#include "JZLog.h"
#include "JZApplication.h"
#include "JZNetClient.h"
#include "JZNetPacket.h"
#include "JZUuid.h"

using namespace std;

JZNetServer::JZNetServer(JZObject *parent)
    : JZNetObject(parent), sigAccept(this), sigDisconnect(this), sigPacketReceive(this)
{   
    m_listen = INVALID_NET_HANDLE;        
}

JZNetServer::~JZNetServer()
{    
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
    return m_listen != INVALID_NET_HANDLE;
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
    info.sslCa = m_sslCa;
    info.sslCert = m_sslCert;
    info.sslKey = m_sslKey;
    m_listen = openConn(info);
    if (!m_listen)
        return false;                    
    
    return true;
}


void JZNetServer::close()
{
    if (m_listen)
    {
        closeConn(m_listen, false);
        m_listen = INVALID_NET_HANDLE;
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
    
    auto id = socket->m_serverId;
    auto it = m_clients.find(id);
    if (it == m_clients.end())
        return;

    m_clients.erase(id);
    sigDisconnect.emit(id);
}

void JZNetServer::onClientPackReceive(JZNetPackPtr ptr)
{
    JZNetClient *socket = (JZNetClient *)sender();
    auto id = socket->m_serverId;
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
    return client->m_serverId;
}

JZNetObject *JZNetServer::onAccept(uint64_t conn)
{
    JZNetClient *socket = new JZNetClient(this);
    socket->m_client = conn;
    socket->m_state = ConnectingState;
    socket->moveToThread(this->thread());
    socket->m_server = this;
    socket->m_serverId = conn;

    connect(&socket->sigDisconnect, this, &JZNetServer::onClientDisconnect);
    connect(&socket->sigPacketReceive, this, &JZNetServer::onClientPackReceive);

    JZObjectPointer socket_ptr = socket->guardPointer();
    auto notify = [this, conn, socket_ptr]
    {
        if (socket_ptr)
        {
            JZNetClient *socket = (JZNetClient *)socket_ptr.object();
            m_clients[conn] = socket;
            m_pendingConnections.push_back(socket);            

            JZNetPackHand hand;
            if (m_token.empty())
            {
                socket->sendHandPacket(&hand);
            }
            else
            {
                // 必须先发送一个没加密的,在加密
                hand.nonce = JZUuid::createUuid().toString();
                socket->sendHandPacket(&hand);

                std::string token = "JZ" + hand.nonce + m_token;
                socket->setToken(token);
                socket->m_packet.setPassword(token);
            }
        }
    };

    runOnObject(this, notify);
    return socket;
}

void JZNetServer::onNetEvent(JZNetResEvent*event)
{
}