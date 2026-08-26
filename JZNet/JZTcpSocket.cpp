#include <string>
#include "JZTcpSocket.h"
#include "JZPlatform.h"
#include "JZLog.h"
#include "JZApplication.h"
#include "JZElapsedTimer.h"

using namespace std;

JZ_OBJECT_IMPL(JZTcpSocket, JZNetObject)

JZTcpSocket::JZTcpSocket(JZObject *parent)
    : JZNetObject(parent), sigConnected(this), sigDisconnected(this), sigReadyRead(this), sigError(this)
{    
    m_conn = INVALID_NET_HANDLE;  
    m_state = UnconnectedState;
}

JZTcpSocket::~JZTcpSocket()
{    
    disconnectFromHost();
}

SocketState JZTcpSocket::state() const
{
    return m_state;
}

bool JZTcpSocket::isConnected() const
{
    return (m_state == ConnectedState);
}

void JZTcpSocket::connectToHost(std::string ip, int port)
{
    JZ_ASSERT(!m_conn);

    JZSocketInfo info;
    info.type = JZSocketInfo::TcpSocket;
    info.url = ip + ":" + to_string(port);
    openConn(info);
    m_state = ConnectingState;
    m_pendingBytes = 0;
}

bool JZTcpSocket::waitForConnected(int ms)
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

void JZTcpSocket::closeInternal(bool is_error)
{
    m_pendingBytes = 0;
    if (m_conn != INVALID_NET_HANDLE)
    {
        closeConn(is_error);
        m_conn = INVALID_NET_HANDLE;
    }
    m_buffer.clear();

    int pre_state = m_state;
    m_state = UnconnectedState;
    if (pre_state == ConnectedState)
        sigDisconnected.emit();
}

void JZTcpSocket::disconnectFromHost()
{
    closeInternal(false);
}

void JZTcpSocket::abort()
{
    closeInternal(true);
}

void JZTcpSocket::close()
{
    disconnectFromHost();
}   

int JZTcpSocket::read(char* buffer, int len)
{
    if (len <= 0) {
        return 0;
    }

    int available = static_cast<int>(m_buffer.size());
    int read_size = (len < available) ? len : available;

    if (read_size > 0) {
        memcpy(buffer, m_buffer.data(), read_size);
        m_buffer = m_buffer.mid(read_size);
    }

    return read_size;
}

JZByteArray JZTcpSocket::readAll()
{
    JZByteArray buffer;
    buffer.swap(m_buffer);
    return buffer;
}

bool JZTcpSocket::waitForReadyRead(int ms)
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

void JZTcpSocket::write(const JZByteArray& buffer)
{
    m_pendingBytes += (int)buffer.size();   // 裸 TCP 数据, 无帧头开销
    writeConn(WriteBin, buffer.data(), buffer.size());
}

void JZTcpSocket::write(const char* buffer, int len)
{
    m_pendingBytes += len;                  // 裸 TCP 数据, 无帧头开销
    writeConn(WriteBin, buffer, len);
}

void JZTcpSocket::onNetEvent(JZNetResEvent *event)
{
    if (m_conn == INVALID_NET_HANDLE)
        return;

    //可能先发送了error, 内部已经断开，后续发close    
    if (m_conn != event->conn && event->event == JZ_NET_EV_CLOSE)        
        return;

    JZ_ASSERT(m_conn == event->conn);

    int ev = event->event;
    if(ev == JZ_NET_EV_CONNECT)
    { 
        m_buffer.clear();
        m_state = ConnectedState;            
        sigConnected.emit();
    }
    else if(ev == JZ_NET_EV_CLOSE)
    {         
        if (m_conn != INVALID_NET_HANDLE)
        {
            sigError.emit("disconnect by server");
            
            m_conn = INVALID_NET_HANDLE;
            closeInternal(false);
        }
    }    
    else if (ev == JZ_NET_EV_READ)
    {        
        m_buffer.append(event->binData.data(), event->binData.size());
        sigReadyRead.emit();
    }
    else if (ev == JZ_NET_EV_WRITE)
    {
        JZDataStream s(&event->binData);
        int32_t write_size = 0;
        s >> write_size;

        m_pendingBytes -= write_size;
        JZ_ASSERT(m_pendingBytes >= 0);
    }
    else if (ev == JZ_NET_EV_ERROR)
    {
        std::string error = str_fromUtf8(event->binData);
        sigError.emit(error);

        if (m_state != UnconnectedState)
            closeInternal(true);
    }
}