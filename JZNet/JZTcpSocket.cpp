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

void JZTcpSocket::endWrite()
{
    if (m_conn == INVALID_NET_HANDLE || m_writeEnded)
        return;

    // 半关闭(对齐 Node 的 socket.end()): **只关写方向, 不再往对端发数据, 读方向继续**。
    // Node 里 `write(x); end();` 之后仍能收到对端回包, 靠的就是这个语义。
    // (原实现 end() 直接 disconnectFromHost() 整个断开 ⇒ 连接立刻没了, 回包全丢。)
    //
    // ⚠ 这里**只打标记, 不发 FIN**(而不是调 JZNetObject::shutdownWrite()):
    //    mongoose 把"读到 0 字节(对端 FIN)"当成整条连接结束(iolog: n<=0 ⇒ is_closing=1),
    //    所以真发 FIN 的话, 对端会**在收到 FIN 的同一轮里被关掉**, 它随后写的东西
    //    (典型: 服务端在 'data' 里回写、而客户端的 `write();end();` 让数据+FIN 同段到达)
    //    会全部丢 —— 实测过, 回包收不到。打标记则对端不受影响, 回包能正常收到;
    //    收尾由应用调 destroy()/close() 完成(Node 那边收尾也常用 destroy())。
    m_writeEnded = true;
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
            // ⚠ 语义修正(对齐 Node): 对端正常关闭**不是** error。
            // Node 的 net.Socket 收到 FIN 只发 'end' + 'close'; 'error' 只留给真错误
            // (ECONNRESET 等, 走下面的 JZ_NET_EV_ERROR)。
            // 原来这里是 sigError.emit("disconnect by server") ⇒ JS 侧先收到一个**假的** 'error'
            // 再收到 'end'/'close' ⇒ 任何 `socket.on('error', reject)` 的 Node 写法都会崩
            // (实测: 基准题 jsapi_net_tcp 里 Codex 的 solution 就是这么崩的)。
            // JS 侧映射: sigDisconnected → 'end' + 'close' (见 JZJSNetModule 的 handle 绑定)。
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