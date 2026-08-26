#include <string>
#include <thread>
#include <sstream>
#include <iomanip>
#include <mongoose.h>
#include "JZFrpClient.h"
#include "JZFrpPacket.h"
#include "JZLog.h"
#include "JZDebug.h"
#include "JZElapsedTimer.h"
#include "JZApplication.h"

using namespace std;

//JZFrpClientConfig
JZFrpClientConfig::JZFrpClientConfig()
{
    serverPort = 7000;
}

void JZFrpClientConfig::addProxy(std::string name, std::string localIp, int localPort, int remotePort)
{
    proxyList.push_back({name, localIp, localPort, remotePort});
}

// JZFrpClient 实现
JZFrpClient::JZFrpClient() 
    : sigConnected(this)
{        
    m_isValid = false;
    m_timer = new JZTimer(this);
    connect(&m_timer->sigTimeout, this, &JZFrpClient::onCheckTimer);
    m_client = nullptr;
}

JZFrpClient::~JZFrpClient() 
{    
    disconnectFromServer();
}

void JZFrpClient::setConfig(JZFrpClientConfig config)
{
    m_config = config;
}

JZFrpClientConfig JZFrpClient::config()
{
    return m_config;
}

void JZFrpClient::connectToServer()
{
    JZ_ASSERT(!m_client);

    m_client = new JZNetClient(this);
    connect(&m_client->sigConnected, this, &JZFrpClient::onConnect);
    connect(&m_client->sigDisconnected, this, &JZFrpClient::onDisconnect);
    connect(&m_client->sigPacketReceive, this, &JZFrpClient::onRecvPacket);
    m_timer->start(15000);

    if (!m_config.token.empty())
        m_client->setToken(m_config.token);
    m_client->connectToHost(m_config.serverIp, m_config.serverPort);
}

void JZFrpClient::disconnectFromServer()
{
    if (!m_client)
        return;

    clearLocalConnects();
    m_client->disconnectFromHost();
    m_client->deleteLater();
    m_client = nullptr;
    m_timer->stop();
    m_isValid = false;
}

bool JZFrpClient::waitForConnected(int ms)
{
    if (!m_client)
        return false;

    JZElapsedTimer t;
    t.start();
    while (!t.hasExpired(ms))
    {
        if (m_client->state() == UnconnectedState)
            return false;
        if (m_isValid)
            return true;

        jzApp->processEvents();
        JZThread::msleep(20);
    }
    return false;
}

void JZFrpClient::clearLocalConnects()
{
    for (auto &it : m_localSocketMap)
    {
        it.first->blockSignals(true);
        it.first->abort();
        it.first->deleteLater();
    }
    m_localSocketMap.clear();
}

JZTcpSocket *JZFrpClient::localClient(int port, int id)
{
    auto it = m_localSocketMap.begin();
    while (it != m_localSocketMap.end())
    {
        if (it->second.serverPort == port && it->second.connectId == id)
            return it->first;

        it++;
    }

    return nullptr;
}

void JZFrpClient::removeLocalClient(JZTcpSocket *c)
{
    c->blockSignals(true);
    c->deleteLater();
    auto it = m_localSocketMap.find(c);
    if (it != m_localSocketMap.end())
    {
        m_localSocketMap.erase(it);
    }
}

void JZFrpClient::removeInvalidClient(JZTcpSocket *c)
{
    LOG_FMT_W("invalid local client %p", c);
    c->disconnectFromHost();
    c->deleteLater();
}

void JZFrpClient::onCheckTimer()
{
    if (!m_client)
        return;

    if (m_client->state() == SocketState::UnconnectedState)
    {
        jzDebug() << "reconnect to server ip:" << m_config.serverIp << ", port:" << m_config.serverPort;
        m_client->connectToHost(m_config.serverIp, m_config.serverPort);
    }
}

void JZFrpClient::onConnect()
{    
    JZFrpHand auth_hand;
    for (size_t i = 0; i < m_config.proxyList.size(); i++)
    {
        auto &proxy = m_config.proxyList[i];
        auth_hand.serverPortList.push_back(proxy.remotePort);
    }
    m_client->sendPacket(&auth_hand);
}

void JZFrpClient::onDisconnect()
{
    clearLocalConnects();
    m_isValid = false;
}

void JZFrpClient::replyInvalidClient(int serverPort, int connectId)
{
    jzWarning("JZFrpClient invalid client, serverPort = %d, connectId = %d", serverPort, connectId);

    // 注意：必须发 FrpPack_Trans(FrpTrans_Error)，服务端才会处理并关闭对应外部连接。
    // 之前发的是 FrpPack_TransRes，而服务端 onRecvPacket 不处理该类型，回执被静默丢弃。
    JZFrpTrans trans_res;
    trans_res.type = FrpTrans_Error;
    trans_res.serverPort = serverPort;
    trans_res.connectId = connectId;
    m_client->sendPacket(&trans_res);
}

void JZFrpClient::onRecvPacket(JZNetPackPtr packet)
{
    int type = packet->packetType();
    if (type == FrpPack_HandRes)
    {
        JZFrpHandRes *hand = (JZFrpHandRes *)packet.get();
        m_isValid = (hand->code == 0);
        if (!m_isValid)
        {
            jzError() << hand->message;
            m_client->disconnectFromHost();
        }
    }
    else if (type == FrpPack_Trans)
    {
        JZFrpTrans *trans = (JZFrpTrans*)packet.get();
        if (trans->type == FrpTrans_Connect)
        {
            int proxy_idx = -1;
            for (size_t i = 0; i < m_config.proxyList.size(); i++)
            {
                if (m_config.proxyList[i].remotePort == trans->serverPort)
                {
                    proxy_idx = i;
                    break;
                }
            }
            if (proxy_idx == -1)
            {
                jzError() << "unexcept server port" << trans->serverPort;
                m_client->disconnectFromHost();
                return;
            }

            string localAddr = m_config.proxyList[proxy_idx].localIp;
            int localPort = m_config.proxyList[proxy_idx].localPort;

            //connect 等待连接成功后发送
            JZTcpSocket *socket = new JZTcpSocket(this);
            socket->connectToHost(localAddr, localPort);
            connect(&socket->sigConnected, this, &JZFrpClient::onLocalConnect);
            connect(&socket->sigDisconnected, this, &JZFrpClient::onLocalDisconnect);
            connect(&socket->sigError, this, &JZFrpClient::onLocalError);
            connect(&socket->sigReadyRead, this, &JZFrpClient::onLocalRead);

            LocalClientInfo client_info;
            client_info.serverPort = trans->serverPort;
            client_info.connectId = trans->connectId;
            m_localSocketMap[socket] = client_info;
        }
        else if (trans->type == FrpTrans_Disconnect)
        {
            auto mg = localClient(trans->serverPort, trans->connectId);
            if (!mg)
            {
                replyInvalidClient(trans->serverPort, trans->connectId);
                return;
            }

            mg->disconnectFromHost();
            mg->deleteLater();
        }
        else if (trans->type == FrpTrans_Write)
        {            
            auto mg = localClient(trans->serverPort, trans->connectId);
            if (!mg) 
            {
                replyInvalidClient(trans->serverPort,trans->connectId);
                return;
            }

            //mg 内部有缓存,这里无需考虑是否连接成功
            mg->write(trans->data);
        }
        else if (trans->type == FrpTrans_Error)
        {
            auto mg = localClient(trans->serverPort, trans->connectId);
            if (!mg)
            {
                replyInvalidClient(trans->serverPort, trans->connectId);
                return;
            }

            if (mg->state() == ConnectingState)
                removeLocalClient(mg);
            else
                mg->abort();     
        }
        else
        {
            jzError() << "unexcept trans message";
            m_client->disconnectFromHost();
        }
    }
}

void JZFrpClient::onLocalConnect()
{
    JZTcpSocket *c = (JZTcpSocket *)sender();
    if (!map_contains(m_localSocketMap, c))
    {
        LOG_FMT_W("invalid local connect %p", c);
        c->disconnectFromHost();
        c->deleteLater();
        return;
    }

    jzDebug("local client connect %p", c);
}

void JZFrpClient::onLocalDisconnect()
{
    JZTcpSocket *c = (JZTcpSocket *)sender();
    if (!map_contains(m_localSocketMap, c))
    {
        removeInvalidClient(c);
        return;
    }

    jzDebug("local client disconnect %p", c);
    LocalClientInfo *config = &m_localSocketMap[c];

    JZFrpTrans trans;
    trans.type = FrpTrans_Disconnect;
    trans.serverPort = config->serverPort;
    trans.connectId = config->connectId;
    m_client->sendPacket(&trans);

    removeLocalClient(c);
}

void JZFrpClient::onLocalRead()
{
    JZTcpSocket *c = (JZTcpSocket *)sender();
    if (!map_contains(m_localSocketMap, c))
    {
        removeInvalidClient(c);
        return;
    }

    LocalClientInfo *config = &m_localSocketMap[c];

    JZFrpTrans trans;
    trans.type = FrpTrans_Write;
    trans.serverPort = config->serverPort;
    trans.connectId = config->connectId;
    trans.data = c->readAll();
    m_client->sendPacket(&trans);
}

void JZFrpClient::onLocalError(const std::string &error)
{
    JZTcpSocket *c = (JZTcpSocket *)sender();
    if (!map_contains(m_localSocketMap, c))
    {
        removeInvalidClient(c);
        return;
    }

    LocalClientInfo *config = &m_localSocketMap[c];

    JZFrpTrans trans;
    trans.type = FrpTrans_Error;
    trans.serverPort = config->serverPort;
    trans.connectId = config->connectId;
    trans.data = str_toUtf8(error);
    m_client->sendPacket(&trans);

    removeLocalClient(c);
}