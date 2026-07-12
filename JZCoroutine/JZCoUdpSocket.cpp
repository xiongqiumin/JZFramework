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
    disconnectFromHost();
}

void JZCoUdpSocket::connectSignals()
{
    connect(&m_socket->sigConnect, this, &JZCoUdpSocket::onConnected);
    connect(&m_socket->sigDisconnect, this, &JZCoUdpSocket::onDisconnected);
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

bool JZCoUdpSocket::connectToHost(std::string ip, int port, int timeout)
{
    if (isConnected())
        return true;

    m_lastError.clear();
    m_socket->connectToHost(ip, port);
    yield(WaitConnect, timeout);
    return isConnected();
}

void JZCoUdpSocket::disconnectFromHost()
{
    m_socket->disconnectFromHost();
}

void JZCoUdpSocket::abort()
{
    m_socket->abort();
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
void JZCoUdpSocket::onConnected()
{
    if (m_waitType == WaitConnect)
        m_coCtrl.wakeAll();
}

void JZCoUdpSocket::onDisconnected()
{
    if (m_waitType != WaitNone)
        m_coCtrl.wakeAll();
}

void JZCoUdpSocket::onError(const std::string &error)
{
    m_lastError = error;
    if (m_waitType != WaitNone)
        m_coCtrl.wakeAll();
}

void JZCoUdpSocket::onReadyRead()
{
    JZCoUdpDatagram dg;
    dg.peerIp = m_socket->peerAddress();
    dg.peerPort = m_socket->peerPort();
    dg.data = m_socket->readAll();
    m_recvQueue.push_back(std::move(dg));
    if (m_waitType == WaitRead)
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
void JZCoUdpSocket::write(const JZByteArray &buffer)
{
    m_socket->write(buffer);
}

void JZCoUdpSocket::write(const char *buffer, int len)
{
    m_socket->write(buffer, len);
}

void JZCoUdpSocket::writeDatagram(const std::string &ip, int port, const JZByteArray &buffer)
{
    m_socket->writeDatagram(ip, port, buffer);
}

void JZCoUdpSocket::writeDatagram(const std::string &ip, int port, const char *buffer, int len)
{
    m_socket->writeDatagram(ip, port, buffer, len);
}

std::string JZCoUdpSocket::peerAddress() const
{
    return m_socket->peerAddress();
}

int JZCoUdpSocket::peerPort() const
{
    return m_socket->peerPort();
}

int JZCoUdpSocket::pendingDatagramCount() const
{
    return static_cast<int>(m_recvQueue.size());
}
