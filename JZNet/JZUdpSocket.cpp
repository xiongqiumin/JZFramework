#include <string>
#include "JZUdpSocket.h"
#include "JZPlatform.h"
#include "JZLog.h"
#include "JZApplication.h"
#include "JZElapsedTimer.h"

using namespace std;

JZ_OBJECT_IMPL(JZUdpSocket, JZNetObject)

JZUdpSocket::JZUdpSocket(JZObject *parent)
    : JZNetObject(parent), sigConnect(this), sigDisconnect(this), sigReadyRead(this), sigError(this)
{
    m_client = INVALID_NET_HANDLE;
    m_state = UnconnectedState;
    m_peerPort = 0;
}

JZUdpSocket::~JZUdpSocket()
{
    disconnectFromHost();
}

SocketState JZUdpSocket::state() const
{
    return m_state;
}

void JZUdpSocket::onNetEvent(JZNetResEvent *event)
{
    int ev = event->event;
    if (ev == JZ_NET_EV_CONNECT)
    {
        m_buffer.clear();
        m_state = ConnectedState;
        sigConnect.emit();
    }
    else if (ev == JZ_NET_EV_CLOSE)
    {
        if (m_client != INVALID_NET_HANDLE)
        {
            m_client = INVALID_NET_HANDLE;
            closeInternal(false);
        }
    }
    else if (ev == JZ_NET_EV_READ)
    {
        m_buffer.append(event->binData.data(), event->binData.size());
        JZUdpResData *udp = (JZUdpResData*)(event->extData.get());
        m_peerAddr = udp->peerIp;
        m_peerPort = udp->peerPort;
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

void JZUdpSocket::connectToHost(std::string ip, int port)
{
    JZ_ASSERT(!m_client);

    JZSocketInfo info;
    info.type = JZSocketInfo::UdpSocket;
    info.url = "udp://" + ip + ":" + to_string(port);
    m_client = openConn(info);
    m_state = ConnectingState;
}

bool JZUdpSocket::waitForConnected(int ms)
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

void JZUdpSocket::closeInternal(bool is_error)
{
    if (m_client != INVALID_NET_HANDLE)
    {
        closeConn(m_client, is_error);
        m_client = INVALID_NET_HANDLE;
    }
    m_buffer.clear();

    int pre_state = m_state;
    m_state = UnconnectedState;
    if (pre_state == ConnectedState)
        sigDisconnect.emit();
}

void JZUdpSocket::disconnectFromHost()
{
    closeInternal(false);
}

void JZUdpSocket::abort()
{
    closeInternal(true);
}

void JZUdpSocket::close()
{
    disconnectFromHost();
}

int JZUdpSocket::read(char* buffer, int len)
{
    if (len <= 0)
        return 0;

    int available = static_cast<int>(m_buffer.size());
    int read_size = (len < available) ? len : available;

    if (read_size > 0)
    {
        memcpy(buffer, m_buffer.data(), read_size);
        m_buffer = m_buffer.mid(read_size);
    }

    return read_size;
}

JZByteArray JZUdpSocket::readAll()
{
    JZByteArray buffer;
    buffer.swap(m_buffer);
    return buffer;
}

bool JZUdpSocket::waitForReadyRead(int ms)
{
    JZElapsedTimer t;
    t.start();
    while (!t.hasExpired(ms))
    {
        if (m_buffer.size() > 0)
            return true;
        else if (m_state == UnconnectedState)
            return false;

        jzApp->processEvents();
        JZThread::msleep(20);
    }
    return false;
}

void JZUdpSocket::write(const JZByteArray &buffer)
{
    write(buffer.data(), buffer.size());
}

void JZUdpSocket::write(const char *buffer, int len)
{
    JZNetReqEvent* req = new JZNetReqEvent();
    req->netCmd = JZNetReqEvent::Net_Write;
    req->writeCmd = WriteUdp;
    req->conn = m_client;
    req->buffer = JZByteArray(buffer, len);
    postReqEvent(req);
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

std::string JZUdpSocket::peerAddress() const
{
    return m_peerAddr;
}

int JZUdpSocket::peerPort() const
{
    return m_peerPort;
}

bool JZUdpSocket::hasPendingDatagrams() const
{
    return m_buffer.size() > 0;
}
