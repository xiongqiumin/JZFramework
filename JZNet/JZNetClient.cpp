#include <string>
#include "JZNetClient.h"
#include "JZPlatform.h"
#include "JZLog.h"
#include "JZApplication.h"
#include "JZElapsedTimer.h"
#include "JZCrypto.h"
#include "JZNetServer.h"
#include "JZDebug.h"

using namespace std;

JZNetClient::JZNetClient(JZObject *parent)
    : JZNetObject(parent), sigConnect(this), sigDisconnect(this), sigPacketReceive(this)
{    
    m_client = INVALID_NET_HANDLE;  
    m_serverId = INVALID_NET_HANDLE;    
    m_waitPacket = false;
    m_state = UnconnectedState;
    m_server = nullptr;
}

JZNetClient::~JZNetClient()
{
    disconnectFromHost();
}

SocketState JZNetClient::state() const
{
    return m_state;
}

uint64_t JZNetClient::serverId() const
{
    return m_serverId;
}

void JZNetClient::setToken(std::string token)
{
    m_token = token;
}

std::string JZNetClient::token()
{
    return m_token;
}

void JZNetClient::setLastError(std::string error)
{
    m_error = error;
}

std::string JZNetClient::lastError()
{
    return m_error;
}

void JZNetClient::clearLastError()
{
    m_error.clear();
}

void JZNetClient::connectToHost(std::string ip, int port)
{
    JZ_ASSERT(!m_client);

    JZSocketInfo info;
    info.type = JZSocketInfo::TcpSocket;
    info.url = ip + ":" + to_string(port);
    m_client = openConn(info);
    m_state = ConnectingState;    
}

bool JZNetClient::waitForConnected(int ms)
{
    JZElapsedTimer t;
    t.start();
    while (!t.hasExpired(ms))
    {
        if (m_state == ConnectedState)
            return true;
        else if (m_state == UnconnectedState)
            return false;

        jzApp->processEvents();
        JZThread::msleep(20);
    }
    return false;
}

bool JZNetClient::isConnect()
{
    return m_state == ConnectedState;
}

void JZNetClient::closeInternal(bool is_error)
{
    if (m_client != INVALID_NET_HANDLE)
    {
        closeConn(m_client, is_error);
        m_client = INVALID_NET_HANDLE;
    }

    m_packet.clearPacket();
    m_waitPacket = false;

    int pre_state = m_state;
    m_state = UnconnectedState;
    if (pre_state == ConnectedState)
        sigDisconnect.emit();
}

void JZNetClient::disconnectFromHost()
{
    if (m_client)
    {
        closeInternal(false);                  
    }
}

void JZNetClient::sendHandPacket(const JZNetPack *packet)
{
    JZByteArray buffer = m_packet.formatPacket(packet);
    writeConn(m_client, WriteBin, buffer.data(), buffer.size());
}

void JZNetClient::sendPacket(const JZNetPack *packet)
{
    if (m_state != ConnectedState)
    {
        jzWarning() << "not connected";
        return;
    }
        
    LOG_NET_D("%s send %d", m_name.c_str(), packet->packetType());

    JZByteArray buffer = m_packet.formatPacket(packet);
    writeConn(m_client, WriteBin, buffer.data(), buffer.size());
}

JZNetPackPtr JZNetClient::waitPacket(int ms)
{
    return waitPacketByType(-1, ms);
}

JZNetPackPtr JZNetClient::waitPacketByType(int packet_type,int ms)
{
    if (!m_client)
        return JZNetPackPtr();

    int64_t t = JZPlatform::getTickCountMs();
    int64_t end = t + ms;

    m_waitPacket = true;
    while (t < end)
    {
        jzApp->processEvents();        

        JZNetPackPtr pkt;
        if(packet_type == -1)
            pkt = m_packet.takePacket();
        else
            pkt = m_packet.takePacketByType(packet_type);

        if (pkt)
        {            
            LOG_NET_D("%s recv %d", m_name.c_str(), pkt->packetType());
            m_waitPacket = false;
            return pkt;
        }

        JZThread::msleep(20);
        t = JZPlatform::getTickCountMs();
    }    
    m_waitPacket = false;
    return JZNetPackPtr();
}

void JZNetClient::clearPacket()
{
    m_packet.clearPacket();    
}


void JZNetClient::onNetEvent(JZNetResEvent *event)
{
    int ev = event->event;
    if (ev == JZ_NET_EV_CONNECT)
    {
        m_packet.clearPacket();
    }
    else if (ev == JZ_NET_EV_CLOSE)
    {
        if (m_client)
        {
            m_client = INVALID_NET_HANDLE;
            closeInternal(false);
        }
    }
    else if (ev == JZ_NET_EV_READ)
    {
        m_packet.append(event->binData.data(), event->binData.size());
        if (!m_packet.parse())
        {
            closeConn(m_client, true);
            disconnectFromHost();
            return;
        }

        if (m_state == ConnectingState)
        {
            auto pkt = m_packet.takePacket();
            if (!pkt)
                return;
            
            if (pkt->packetType() == NetPack_hand)
            {
                JZNetPackHand *hand = (JZNetPackHand *)pkt.get();
                if (hand->nonce.empty() != m_token.empty())
                {
                    jzWarning() << "client server token not equal";
                    closeInternal(true);
                }
                else
                {
                    if (!hand->nonce.empty())
                    {
                        std::string token = "JZ" + hand->nonce + m_token;
                        m_packet.setPassword(token);
                    }

                    JZNetPackHandRes hand_res;
                    sendHandPacket(&hand_res);

                    m_state = ConnectedState;
                    sigConnect.emit();
                }
            }
            else if (pkt->packetType() == NetPack_handRes)
            {
                uint64_t c = map_key(m_server->m_clients, this, INVALID_NET_HANDLE);
                if (c != INVALID_NET_HANDLE)
                {
                    m_state = ConnectedState;
                    m_server->m_pendingConnections.push_back(this);
                    m_server->sigAccept.emit();
                }
                else
                {
                    closeConn(m_client, true);
                }
            }
            else
            {
                jzWarning() << "unknow pacekt in connecting state" << pkt->packetType(); 
                closeInternal(true);
            }

            if (m_state != ConnectedState)
                return;
        }

        while (!m_waitPacket)
        {
            auto pkt = m_packet.takePacket();
            if (pkt)
            {
                LOG_NET_D("%s recv %d", m_name.c_str(), pkt->packetType());
                sigPacketReceive.emit(pkt);
            }
            else
                break;
        }
    }
    else if (ev == JZ_NET_EV_ERROR)
    {
        closeInternal(true);
    }
}