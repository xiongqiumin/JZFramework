#include "JZCoTcpSocket.h"
#include "JZElapsedTimer.h"

JZCoTcpSocket::JZCoTcpSocket(JZObject *parent)
    : JZObject(parent)
    , m_waitType(WaitNone)
{
    m_socket = new JZTcpSocket(this);
    connectSignals();
}

JZCoTcpSocket::~JZCoTcpSocket()
{
    disconnectFromHost();
}

void JZCoTcpSocket::connectSignals()
{
    connect(&m_socket->sigConnected, this, &JZCoTcpSocket::onConnected);
    connect(&m_socket->sigDisconnected, this, &JZCoTcpSocket::onDisconnected);
    connect(&m_socket->sigReadyRead, this, &JZCoTcpSocket::onReadyRead);
}

void JZCoTcpSocket::yield(WaitType type, int timeout)
{
    m_waitType = type;
    m_coCtrl.wait(timeout);
    m_waitType = WaitNone;
}

bool JZCoTcpSocket::connectToHost(std::string ip, int port, int timeout)
{
    if (isConnected())
        return true;

    m_socket->connectToHost(ip, port);
    yield(WaitConnect, timeout);
    return isConnected();
}

void JZCoTcpSocket::disconnectFromHost()
{
    m_socket->disconnectFromHost();
}

SocketState JZCoTcpSocket::state() const
{
    return m_socket->state();
}

bool JZCoTcpSocket::isConnected() const
{
    return state() == SocketState::ConnectedState;
}

//=======================================================================
// 信号处理
//=======================================================================
void JZCoTcpSocket::onConnected()
{
    if (m_waitType == WaitConnect)
        m_coCtrl.wakeAll();
}

void JZCoTcpSocket::onDisconnected()
{
    if (m_waitType != WaitNone)
        m_coCtrl.wakeAll();
}

void JZCoTcpSocket::onReadyRead()
{
    m_recvBuffer.append(m_socket->readAll());
    if (m_waitType == WaitRead)
        m_coCtrl.wakeAll();
}

bool JZCoTcpSocket::waitForReadyRead(int timeout)
{
    if (!isConnected())
        return false;

    yield(WaitRead, timeout);
    return isConnected();
}

//=======================================================================
//  readAll → JZResult
//=======================================================================
JZResult<JZByteArray> JZCoTcpSocket::readAll(int ms)
{
    if (!m_recvBuffer.isEmpty()) {
        JZByteArray buf = m_recvBuffer;
        m_recvBuffer.clear();
        return JZResult<JZByteArray>::Ok(buf);
    }

    if (!waitForReadyRead(ms)) {
        return JZResult<JZByteArray>::Error("disconnected");
    }

    JZByteArray buf = m_recvBuffer;
    m_recvBuffer.clear();
    return JZResult<JZByteArray>::Ok(buf);
}

//=======================================================================
//  read → JZResult
//=======================================================================
JZResult<JZByteArray> JZCoTcpSocket::read(int len, int ms)
{
    if (len <= 0)
        return JZResult<JZByteArray>::Error("invalid length");

    JZElapsedTimer t;
    t.start();
    JZByteArray res;

    while (res.size() < len) {
        if (!m_recvBuffer.isEmpty()) {
            int need = len - res.size();
            int take = std::min(need, (int)m_recvBuffer.size());
            res.append(m_recvBuffer.data(), take);
            m_recvBuffer = m_recvBuffer.mid(take);
            continue;
        }

        if (!isConnected())
            return JZResult<JZByteArray>::Error("disconnected");

        int waitMs = ms;
        if (t.elapsed() >= ms) {
            return JZResult<JZByteArray>::Error("timeout");
        }

        if (!waitForReadyRead(waitMs))
            return JZResult<JZByteArray>::Error("disconnected");
    }

    return JZResult<JZByteArray>::Ok(res);
}

void JZCoTcpSocket::write(const JZByteArray &buffer)
{
    m_socket->write(buffer);
}

void JZCoTcpSocket::write(const char *buffer, int len)
{
    m_socket->write(buffer, len);
}