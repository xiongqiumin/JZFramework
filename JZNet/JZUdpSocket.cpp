#include <string>
#include "JZUdpSocket.h"
#include "JZPlatform.h"
#include "JZLog.h"
#include "JZApplication.h"

using namespace std;

JZ_OBJECT_IMPL(JZUdpSocket, JZNetObject)

JZUdpSocket::JZUdpSocket(JZObject *parent)
    : JZNetObject(parent), sigReadyRead(this), sigError(this)
{
}

JZUdpSocket::~JZUdpSocket()
{
    close();
}

SocketState JZUdpSocket::state() const
{
    return m_state;
}

void JZUdpSocket::onNetEvent(JZNetResEvent *event)
{
    int ev = event->event;
    if (ev == JZ_NET_EV_CLOSE)
    {
        if (m_client != INVALID_NET_HANDLE)
        {
            m_client = INVALID_NET_HANDLE;
            closeInternal(false);
        }
    }
    else if (ev == JZ_NET_EV_READ)
    {
        JZUdpResData *udp = (JZUdpResData*)(event->extData.get());
        JZUdpDatagram dgram;
        dgram.data = JZByteArray(event->binData.data(), event->binData.size());
        dgram.peerIp = udp ? udp->peerIp : std::string();
        dgram.peerPort = udp ? udp->peerPort : 0;
        m_datagrams.push_back(std::move(dgram));
        sigReadyRead.emit();
    }
    else if (ev == JZ_NET_EV_ERROR)
    {
        std::string error = str_fromUtf8(event->binData);
        sigError.emit(error);

        if (m_state == ConnectingState)
            closeInternal(true);
    }
}

bool JZUdpSocket::bind(int port)
{
    JZ_ASSERT(!m_client);

    JZSocketInfo info;
    info.type = JZSocketInfo::UdpListen;
    info.url = "udp://:" + to_string(port);
    m_client = openConn(info);
    if (m_client == INVALID_NET_HANDLE)
        return false;

    m_state = ConnectedState;
    return true;
}

void JZUdpSocket::closeInternal(bool is_error)
{
    if (m_client != INVALID_NET_HANDLE)
    {
        closeConn(m_client, is_error);
        m_client = INVALID_NET_HANDLE;
    }
    m_datagrams.clear();

    m_state = UnconnectedState;
}

void JZUdpSocket::close()
{
    closeInternal(false);
}

void JZUdpSocket::writeDatagram(const std::string &ip, int port, const JZByteArray &buffer)
{
    writeDatagram(ip, port, buffer.data(), buffer.size());
}

void JZUdpSocket::writeDatagram(const std::string &ip, int port, const char *buffer, int len)
{
    std::shared_ptr<JZUdpWriteData> data = std::make_shared<JZUdpWriteData>();
    data->ip = ip;
    data->port = port;

    JZNetReqEvent* req = new JZNetReqEvent();
    req->netCmd = JZNetReqEvent::Net_Write;
    req->writeCmd = WriteUdp;
    req->conn = m_client;
    req->buffer = JZByteArray(buffer, len);
    req->extData = data;
    postReqEvent(req);
}

JZUdpDatagram JZUdpSocket::readDatagram()
{
    if (m_datagrams.empty())
        return JZUdpDatagram();

    JZUdpDatagram dgram = std::move(m_datagrams.front());
    m_datagrams.pop_front();

    m_peerAddr = dgram.peerIp;
    m_peerPort = dgram.peerPort;

    return dgram;
}

bool JZUdpSocket::hasPendingDatagrams() const
{
    return !m_datagrams.empty();
}
