#include "JZCoUdpSocket.h"

JZCoUdpSocket::JZCoUdpSocket(JZObject *parent)
    : JZObject(parent)
    , m_waitType(WaitNone)
{
    m_socket = new JZUdpSocket(this);
    connectSignals();
}

JZCoUdpSocket::~JZCoUdpSocket()
{
    m_socket->close();
}

void JZCoUdpSocket::connectSignals()
{
    // 新的 JZUdpSocket 仅提供 sigReadyRead / sigError 两个信号
    connect(&m_socket->sigReadyRead, this, &JZCoUdpSocket::onReadyRead);
    connect(&m_socket->sigError, this, &JZCoUdpSocket::onError);
}

void JZCoUdpSocket::yield(WaitType type, int timeout)
{
    m_waitType = type;
    m_coCtrl.wait(timeout);
    m_waitType = WaitNone;
}

bool JZCoUdpSocket::bind(int port)
{
    return m_socket->bind(port);
}

void JZCoUdpSocket::close()
{
    m_socket->close();
}

SocketState JZCoUdpSocket::state() const
{
    return m_socket->state();
}

bool JZCoUdpSocket::isConnected() const
{
    return state() == SocketState::ConnectedState;
}

//=======================================================================
//  信号处理
//=======================================================================
void JZCoUdpSocket::onReadyRead()
{
    // 新的 JZUdpSocket 在内部用 deque 缓冲多个数据报, 一次 sigReadyRead 可能到达
    // 多个数据报。这里全部取入接收队列, 否则剩余数据报不会再次触发 sigReadyRead,
    // 导致 receive() 永久挂起。
    while (m_socket->hasPendingDatagrams())
    {
        JZUdpDatagram dgram = m_socket->readDatagram();
        JZCoUdpDatagram dg;
        dg.peerIp = dgram.peerIp;
        dg.peerPort = dgram.peerPort;
        dg.data = dgram.data;
        m_lastPeerIp = dgram.peerIp;
        m_lastPeerPort = dgram.peerPort;
        m_recvQueue.push_back(std::move(dg));
    }
    if (m_waitType == WaitRead)
        m_coCtrl.wakeAll();
}

void JZCoUdpSocket::onError(const std::string &error)
{
    m_lastError = error;
    if (m_waitType != WaitNone)
        m_coCtrl.wakeAll();
}

bool JZCoUdpSocket::waitForReadyRead(int timeout)
{
    if (!m_recvQueue.empty())
        return true;

    if (!isConnected())
        return false;

    yield(WaitRead, timeout);
    return !m_recvQueue.empty();
}

//=======================================================================
//  receive → 单个数据报
//=======================================================================
JZResult<JZCoUdpDatagram> JZCoUdpSocket::receive(int ms)
{
    if (!m_recvQueue.empty())
    {
        JZCoUdpDatagram dg = std::move(m_recvQueue.front());
        m_recvQueue.erase(m_recvQueue.begin());
        return JZResult<JZCoUdpDatagram>::Ok(std::move(dg));
    }

    if (!waitForReadyRead(ms))
        return JZResult<JZCoUdpDatagram>::Error(!m_lastError.empty() ? m_lastError : (isConnected() ? "timeout" : "disconnected"));

    JZCoUdpDatagram dg = std::move(m_recvQueue.front());
    m_recvQueue.erase(m_recvQueue.begin());
    return JZResult<JZCoUdpDatagram>::Ok(std::move(dg));
}

//=======================================================================
//  发送
//=======================================================================
void JZCoUdpSocket::writeDatagram(const std::string &ip, int port, const JZByteArray &buffer)
{
    m_socket->writeDatagram(ip, port, buffer);
}

void JZCoUdpSocket::writeDatagram(const std::string &ip, int port, const char *buffer, int len)
{
    m_socket->writeDatagram(ip, port, buffer, len);
}

bool JZCoUdpSocket::hasPendingDatagrams() const
{
    return m_socket->hasPendingDatagrams();
}
