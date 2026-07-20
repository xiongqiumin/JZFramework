#include <string>
#include <thread>
#include <algorithm>
#include <random>
#include <sstream>
#include <iomanip>
#include <mongoose.h>
#include "JZFrpServer.h"
#include "JZNetClient.h"
#include "JZLog.h"
#include "JZDebug.h"
#include "JZUuid.h"

using namespace std;

//RemoteInfo
JZFrpServer::RemoteInfo::RemoteInfo()
{
    listen = nullptr;
    port = 0;
    clientId = 0;
}

JZFrpServer::RemoteInfo::~RemoteInfo()
{
    JZ_ASSERT(!listen && remoteConnects.size() == 0);
}

void JZFrpServer::RemoteInfo::close()
{
    auto tmp_connects = remoteConnects;
    for (auto &it : tmp_connects)
    {
        it.second->disconnectFromHost();
        it.second->deleteLater();
    }
    remoteConnects.clear();

    if (listen)
    {
        listen->close();
        listen->deleteLater();
        listen = nullptr;
    }
}

//FrpConnection
JZFrpServer::FrpConnection::FrpConnection()
{
    valid = false;
    frpc = INVALID_NET_HANDLE;
}

JZFrpServer::FrpConnection::~FrpConnection()
{
}

void JZFrpServer::FrpConnection::close(JZFrpServer *server)
{
    auto tmp_frps = frps;
    for (auto &frps_it : tmp_frps)
    {
        frps_it.second->close();
    }
    frps.clear();

    if (frpc != INVALID_NET_HANDLE)
    {
        server->m_server->closeClient(frpc);
        frpc = INVALID_NET_HANDLE;
    }
}

JZTcpSocket *JZFrpServer::FrpConnection::remoteClient(int port, int id)
{
    auto it = frps.find(port);
    if (it == frps.end())
        return nullptr;

    return map_value(it->second->remoteConnects, id);
}

// JZFrpServer 实现
JZFrpServer::RemoteClientInfo::RemoteClientInfo()
{    
    serverPort = 0;
    connectId = 0;
}

JZFrpServer::JZFrpServer(JZObject *parent) 
    : JZObject(parent)
{
    m_server = new JZNetServer(parent);
    connect(&m_server->sigAccept, this, &JZFrpServer::onNewAccept);
    connect(&m_server->sigDisconnect, this, &JZFrpServer::onDisconnect);
    connect(&m_server->sigPacketReceive, this, &JZFrpServer::onRecvPacket);

}

JZFrpServer::~JZFrpServer() 
{
    close();
}

void JZFrpServer::setConfig(JZFrpServerConfig config)
{
    m_config = config;
    if (!m_config.token.empty())
        m_server->setToken(m_config.token);
}

bool JZFrpServer::listen(int port)
{
    return m_server->listen(port);
}

void JZFrpServer::close()
{
    closeRemoteClients();

    m_server->close();
}

JZFrpServer::FrpConnection *JZFrpServer::getFrpClient(uint64_t c)
{
    return map_value(m_frpClient, c).get();
}

JZFrpServer::FrpConnection *JZFrpServer::getFrpClientByPort(int port)
{
    for (auto &client : m_frpClient)
    {
        for (auto &it : client.second->frps)
        {
            if (it.first == port)
                return client.second.get();
        }
    }
    return nullptr;
}

void JZFrpServer::closeRemoteClients()
{
    auto tmp_clients = m_frpClient;
    auto client_it = tmp_clients.begin();
    while (client_it != tmp_clients.end())
    {
        client_it->second->close(this);
        client_it++;
    }
    m_frpClient.clear();
    m_remoteInfo.clear();
}

void JZFrpServer::onNewAccept()
{    
    uint64_t client = m_server->nextPendingConnection();
    if (!client)
        return;

    auto frp = std::make_shared<FrpConnection>();
    frp->frpc = client;
    m_frpClient[client] = frp;

    jzDebug() << "FrpServer new connect" << client;
}

void JZFrpServer::onDisconnect(uint64_t c)
{    
    auto client_it = m_frpClient.find(c);
    if (client_it == m_frpClient.end())
        return;

    client_it->second->frpc = INVALID_NET_HANDLE;
    client_it->second->close(this);
    m_frpClient.erase(client_it);        
    jzDebug() << "FrpServer disconnect" << c;
}

void JZFrpServer::onRecvPacket(uint64_t c, JZNetPackPtr packet)
{
    auto frp = getFrpClient(c);
    if (!frp)
    {
        LOG_FMT_W("JZFrpServer invalid frp client %llu", (unsigned long long)c);
        m_server->closeClient(c);
        return;
    }

    int type = packet->packetType();
    if (type == FrpPack_Hand)
    {
        if (frp->valid)
        {
            m_server->closeClient(c);
            return;
        }

        std::string error;
        JZFrpHand* pack_hand = (JZFrpHand*)packet.get();
        for (int i = 0; i < pack_hand->serverPortList.size(); i++)
        {
            int serverPort = pack_hand->serverPortList[i];
            if (frp->frps.count(serverPort))
            {
                error = "duplicate server port " + to_string(serverPort);
                break;
            }

            auto mgr_info = std::make_shared<RemoteInfo>();
            frp->frps[serverPort] = mgr_info;
            mgr_info->port = serverPort;
            mgr_info->listen = new JZTcpServer(this);
            connect(&mgr_info->listen->sigAccept, this, &JZFrpServer::onFrpcAccept);
            if (!mgr_info->listen->listen(serverPort))
            {
                error = "listen port " + to_string(serverPort) + " failed";
                break;
            }
        }

        JZFrpHandRes res;
        res.code = error.empty() ? 0 : 1;
        res.message = error;
        m_server->sendPacket(c, &res);

        frp->valid = (res.code == 0);
        if (!frp->valid)
        {
            frp->close(this);
        }
        return;
    }
    if (!frp->valid)
    {
        LOG_FMT_W("JZFrpServer invalid connect %p", c);
        m_server->closeClient(c);
        return;
    }

    if (type == FrpPack_Trans)
    {
        JZFrpTrans* pack_trans = (JZFrpTrans*)packet.get();

        if (pack_trans->type == FrpTrans_Disconnect)
        {            
            auto *remote_c = frp->remoteClient(pack_trans->serverPort, pack_trans->connectId);
            if (remote_c)
            {
                remote_c->disconnectFromHost();
                remote_c->deleteLater();
            }
        }
        else if (pack_trans->type == FrpTrans_Write)
        {
            auto *remote_c = frp->remoteClient(pack_trans->serverPort, pack_trans->connectId);
            if (remote_c)
            {
                remote_c->write(pack_trans->data);
            }
        }        
        else if (pack_trans->type == FrpTrans_Error)
        {
            auto *remote_c = frp->remoteClient(pack_trans->serverPort, pack_trans->connectId);
            if (remote_c)
            {
                remote_c->abort();
                remote_c->deleteLater();

                auto frps_it = frp->frps.find(pack_trans->serverPort);
                if (frps_it != frp->frps.end())
                    map_remove(frps_it->second->remoteConnects, pack_trans->connectId);
            }
        }
        else
        {
            LOG_FMT_W("JZFrpServer FrpPack_Trans received unexpected type");
            m_server->closeClient(c);
            return;
        }
    }
}

void JZFrpServer::onFrpcAccept()
{        
    JZTcpServer *server = (JZTcpServer *)sender();
    int port = server->port();
    JZTcpSocket *c = server->nextPendingConnection();
    if (!c)
        return;
    
    FrpConnection *frp = getFrpClientByPort(port);
    JZ_ASSERT(frp);

    auto frps_it = frp->frps.find(port);
    JZ_ASSERT(frps_it != frp->frps.end());

    auto mgr_info = frps_it->second.get();
    int connect_id = mgr_info->clientId++;

    RemoteClientInfo remote_info;
    remote_info.connectId = connect_id;
    remote_info.serverPort = port;
    m_remoteInfo[c] = remote_info;
    connect(&c->sigDisconnect, this, &JZFrpServer::onFrpcClose);
    connect(&c->sigReadyRead, this, &JZFrpServer::onFrpcRead);
    
    mgr_info->remoteConnects[connect_id] = c;

    JZFrpTrans packet;
    packet.type = FrpTrans_Connect;
    packet.serverPort = port;
    packet.connectId = connect_id;
    m_server->sendPacket(frp->frpc, &packet);
}

void JZFrpServer::onFrpcRead()
{
    JZTcpSocket *c = (JZTcpSocket *)sender();

    RemoteClientInfo *remote_info = &m_remoteInfo[c];
    FrpConnection *frp = getFrpClientByPort(remote_info->serverPort);

    JZFrpTrans packet;
    packet.type = FrpTrans_Write;
    packet.serverPort = remote_info->serverPort;
    packet.connectId = remote_info->connectId;
    packet.data = c->readAll();    
    m_server->sendPacket(frp->frpc, &packet);
}

void JZFrpServer::onFrpcClose()
{   
    JZTcpSocket *c = (JZTcpSocket *)sender();

    if (!map_contains(m_remoteInfo,c))
        return;

    RemoteClientInfo *info = &m_remoteInfo[c];
    FrpConnection *frp = getFrpClientByPort(info->serverPort);
    if (frp)
    {
        JZFrpTrans packet;
        packet.type = FrpTrans_Disconnect;
        packet.serverPort = info->serverPort;
        packet.connectId = info->connectId;
        m_server->sendPacket(frp->frpc, &packet);

        auto frps_it = frp->frps.find(info->serverPort);
        if (frps_it != frp->frps.end())
        {
            map_remove(frps_it->second->remoteConnects, info->connectId);
        }
    }

    c->disconnectFromHost();
    c->deleteLater();
    map_remove(m_remoteInfo, c);
}