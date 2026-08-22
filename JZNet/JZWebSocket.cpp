#include "JZApplication.h"
#include "JZElapsedTimer.h"
#include "JZDebug.h"
#include "JZPlatform.h"
#include "JZWebSocket.h"

//JZWebSocket
JZ_OBJECT_IMPL(JZWebSocket, JZNetObject)

JZWebSocket::JZWebSocket(JZObject *parent)
    :JZNetObject(parent), sigConnected(this), sigDisconnected(this), sigTextMessageReceived(this),
        sigBinaryMessageReceived(this), sigError(this),
        m_pingTimer(this)
{
    m_conn = INVALID_NET_HANDLE;
    m_state = UnconnectedState;
    m_pingTimer.setSingleShot(false);
    connect(&m_pingTimer.sigTimeout, this, &JZWebSocket::onPingTick);
}

JZWebSocket::~JZWebSocket()
{
    disconnectFromHost();
}

SocketState JZWebSocket::state() const
{
    return m_state;
}

bool JZWebSocket::isConnected() const
{
    return m_state == ConnectedState;
}

void JZWebSocket::connectToHost(const std::string &uri)
{
    connectToHost(JZNetworkRequest(JZUrl(uri)));
}

void JZWebSocket::connectToHost(const JZNetworkRequest &req)
{
    JZ_ASSERT(!m_conn);

    JZSocketInfo info;
    info.type = JZSocketInfo::WebSocket;
    info.url = req.url().url();
    info.websocketExt = req.buildWebSocketHeader();
    openConn(info);
    m_state = ConnectingState;    
}

void JZWebSocket::closeInternal(bool flag)
{
    stopAutoPing();
    if (m_conn != INVALID_NET_HANDLE)
    {
        closeConn(flag);
        m_conn = INVALID_NET_HANDLE;
    }

    int pre_state = m_state;
    m_state = UnconnectedState;
    if (pre_state == ConnectedState)
        sigDisconnected.emit();
}

void JZWebSocket::disconnectFromHost()
{
    if (m_conn != INVALID_NET_HANDLE)
    {
        closeInternal(false);
    }
}

void JZWebSocket::setAutoPing(bool enable, int intervalMs, int timeoutMs)
{
    m_autoPing = enable;
    m_pingIntervalMs = intervalMs;
    m_pingTimeoutMs = timeoutMs;
    if (!m_autoPing)
        stopAutoPing();
    else if (m_state == ConnectedState)
        startAutoPing();
}

void JZWebSocket::sendPing()
{
    if (m_conn == INVALID_NET_HANDLE)
        return;
    // PING 帧负载可为空, 这里发一个空 payload
    writeConn(WriteWebSocketPing, "", 0);
}

void JZWebSocket::startAutoPing()
{
    if (!m_autoPing)
        return;

    int64_t now = JZPlatform::getTickCountMs();    
    m_lastPingMs = now;    
    m_recvPong = true;
    m_pingTimer.start(1000);    
}

void JZWebSocket::stopAutoPing()
{
    m_pingTimer.stop();
}

void JZWebSocket::onPingTick()
{
    if (m_conn == INVALID_NET_HANDLE || m_state != ConnectedState)
    {
        m_pingTimer.stop();
        return;
    }

    int64_t now = JZPlatform::getTickCountMs();
    int64_t elapsed = now - m_lastPingMs;

    // 1. 【优先级最高】如果发了 Ping 且超时未收到 Pong -> 立即断开
    //    注意：这里不判断 elapsed >= m_pingIntervalMs，只判断超时
    if (!m_recvPong && elapsed >= m_pingTimeoutMs)
    {
        sigError.emit();
        closeInternal(true);
        return;
    }

    // 2. 只有【收到了上次 Pong】且【达到间隔】时，才发送新的 Ping
    if (m_recvPong && elapsed >= m_pingIntervalMs)
    {
        m_lastPingMs = now;
        m_recvPong = false;   // 等待新的 Pong
        sendPing();
    }
}

void JZWebSocket::onPongReceived()
{
    m_recvPong = true;
}

bool JZWebSocket::waitForConnected(int ms)
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

void JZWebSocket::sendTextMessage(const std::string &message)
{
    if (m_conn == INVALID_NET_HANDLE)
        return;

    // WebSocket 帧头开销: 长度 <126 -> 2字节, <65536 -> 4字节, 否则 10字节
    // client 端额外带 4 字节掩码(与回执 write_size=mg_ws_send 返回值口径一致)
    size_t len = message.size();
    size_t hdr = (len < 126) ? 2 : (len < 65536 ? 4 : 10);
    if (m_connType == JZSocketInfo::WebSocket)
        hdr += 4;
    m_pendingBytes += (int)(len + hdr);
    
    writeConn(WriteWebSocketText, message.data(), (int)message.size());
}

void JZWebSocket::sendBinaryMessage(const JZByteArray &message)
{
    if (m_conn == INVALID_NET_HANDLE)
        return;

    size_t len = message.size();
    size_t hdr = (len < 126) ? 2 : (len < 65536 ? 4 : 10);
    if (m_connType == JZSocketInfo::WebSocket)
        hdr += 4;
    m_pendingBytes += (int)(len + hdr);

    writeConn(WriteWebSocketBin, message.data(), message.size());
}

void JZWebSocket::onNetEvent(JZNetResEvent*event)
{
    if (m_conn == INVALID_NET_HANDLE)
        return;
    
    //可能先发送了error, 内部已经断开，后续发close
    if (m_conn != event->conn && event->event == JZ_NET_EV_CLOSE)        
        return;

    JZ_ASSERT(m_conn == event->conn);
    if (event->event == JZ_NET_EV_WS_OPEN)
    {
        m_state = ConnectedState;
        sigConnected.emit();
        startAutoPing();
    }
    else if (event->event == JZ_NET_EV_WS_MSG)
    {        
        JZWsResData *ws = (JZWsResData*)(event->extData.get());
        int op = ws->wsFlag & 15;
        if (op == JZ_WEBSOCKET_OP_PONG) {
            // 对端回应了 pong, 连接存活
            onPongReceived();
        }
        else if (op == JZ_WEBSOCKET_OP_TEXT) {            
            sigTextMessageReceived.emit(str_fromUtf8(event->binData));
        }
        else if (op == JZ_WEBSOCKET_OP_BINARY) {
            sigBinaryMessageReceived.emit(event->binData);
        }
    }
    else if (event->event == JZ_NET_EV_WRITE)
    {
        if(m_state == ConnectedState)
        {
            JZDataStream s(&event->binData);
            int32_t write_size = 0;
            s >> write_size;
            
            m_pendingBytes -= write_size;
            if(m_pendingBytes < 0) //可能有握手ping pong之类的
                m_pendingBytes = 0;
        }
    }
    else if (event->event == JZ_NET_EV_CLOSE)
    {                
        if (m_conn != INVALID_NET_HANDLE)
        {
            sigError.emit();

            m_conn = INVALID_NET_HANDLE;
            closeInternal(false);
        }
    }
    else if (event->event == JZ_NET_EV_ERROR)
    {        
        std::string error_str = str_fromUtf8(event->binData);
        sigError.emit();

        if (m_state != UnconnectedState)
            closeInternal(true);
    }
}