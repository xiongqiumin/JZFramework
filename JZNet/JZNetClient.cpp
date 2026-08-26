#include <string>
#include "JZNetClient.h"
#include "JZPlatform.h"
#include "JZLog.h"
#include "JZApplication.h"
#include "JZElapsedTimer.h"
#include "JZCrypto.h"
#include "JZNetServer.h"
#include "JZDebug.h"
#include "JZUuid.h"

using namespace std;

JZNetClient::JZNetClient(JZObject *parent)
    : JZNetObject(parent), sigConnected(this), sigDisconnected(this), sigPacketReceive(this)
{    
    m_conn = INVALID_NET_HANDLE;  
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
    JZ_ASSERT(!m_conn);

    JZSocketInfo info;
    info.type = JZSocketInfo::TcpSocket;
    info.url = ip + ":" + to_string(port);
    openConn(info);
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

bool JZNetClient::isConnected()
{
    return m_state == ConnectedState;
}

void JZNetClient::closeInternal(bool is_error)
{
    if (m_conn != INVALID_NET_HANDLE)
    {
        closeConn(is_error);
        m_conn = INVALID_NET_HANDLE;
    }

    m_packet.clearPacket();
    m_waitPacket = false;

    int pre_state = m_state;
    m_state = UnconnectedState;
    if (pre_state == ConnectedState)
        sigDisconnected.emit();
}

void JZNetClient::disconnectFromHost()
{
    if (m_conn)
    {
        closeInternal(false);                  
    }
}

void JZNetClient::sendHandPacket(const JZNetPack *packet)
{
    JZByteArray buffer = m_packet.formatPacket(packet);
    writeConn(WriteBin, buffer.data(), buffer.size());
}

void JZNetClient::sendPacket(const JZNetPack *packet)
{
    if (m_state != ConnectedState)
    {
        jzWarning() << "not connected";
        return;
    }
    
    JZByteArray buffer = m_packet.formatPacket(packet);
    writeConn(WriteBin, buffer.data(), buffer.size());
}

JZNetPackPtr JZNetClient::waitPacket(int ms)
{
    return waitPacketByType(-1, ms);
}

JZNetPackPtr JZNetClient::waitPacketByType(int packet_type,int ms)
{
    if (!m_conn)
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
    if (m_conn == INVALID_NET_HANDLE)
        return;

    //可能先发送了error, 内部已经断开，后续发close    
    if (m_conn != event->conn && event->event == JZ_NET_EV_CLOSE)        
        return;

    JZ_ASSERT(m_conn == event->conn);
    
    int ev = event->event;
    if (ev == JZ_NET_EV_CONNECT)
    {
        m_packet.clearPacket();

        // 客户端主动发起握手,先给服务器发送 Hand
        if (m_server == nullptr && m_state == ConnectingState)
        {
            JZNetPackHand hand;
            if (!m_token.empty())
            {
                // 必须先发送一个没加密的,在加密
                hand.nonce = JZUuid::createUuid().toString();
            }
            sendHandPacket(&hand);
        }
    }
    else if (ev == JZ_NET_EV_CLOSE)
    {
        if (m_conn)
        {
            m_conn = INVALID_NET_HANDLE;
            closeInternal(false);
        }
    }
    else if (ev == JZ_NET_EV_READ)
    {
        m_packet.append(event->binData.data(), event->binData.size());
        if (!m_packet.parse())
        {
            closeConn(true);
            disconnectFromHost();
            return;
        }

        if (m_state == ConnectingState)
        {
            auto pkt = m_packet.takePacket();
            if (!pkt)
                return;

            if (m_server == nullptr)
            {
                // 客户端侧:等待服务器的握手回包
                if (pkt->packetType() == NetPack_handRes)
                {
                    m_state = ConnectedState;
                    sigConnected.emit();
                }
                else
                {
                    jzWarning() << "unknow packet in connecting state" << pkt->packetType();
                    closeInternal(true);
                }
            }
            else
            {
                // 服务器侧:收到客户端的 Hand,校验后回 HandRes
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
                        m_server->m_pendingConnections.push_back(this);
                        m_server->sigAccept.emit();
                    }
                }
                else
                {
                    jzWarning() << "unknow packet in connecting state" << pkt->packetType();
                    closeInternal(true);
                }
            }

            if (m_state != ConnectedState)
                return;
        }

        while (!m_waitPacket)
        {
            auto pkt = m_packet.takePacket();
            if (pkt)
            {
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