#include "JZApplication.h"
#include "JZElapsedTimer.h"
#include "JZDebug.h"
#include "JZWebSocket.h"

//JZWebSocket
JZ_OBJECT_IMPL(JZWebSocket, JZNetObject)

JZWebSocket::JZWebSocket(JZObject *parent)
    :JZNetObject(parent), sigConnected(this), sigDisconnected(this), sigTextMessageReceived(this),
        sigBinaryMessageReceived(this), sigError(this)
{
    m_conn = INVALID_NET_HANDLE;
    m_state = UnconnectedState;
}

JZWebSocket::~JZWebSocket()
{
    disconnectFromHost();
}

SocketState JZWebSocket::state() const
{
    return m_state;
}

bool JZWebSocket::isConnect() const
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
    info.sslCa = m_sslCa;
    info.sslCert = m_sslCert;
    info.sslKey = m_sslKey;
    info.sslSkipVerify = m_sslSkipVerify;
    m_conn = openConn(info);
    m_state = ConnectingState;    
}

void JZWebSocket::closeInternal(bool flag)
{
    if (m_conn != INVALID_NET_HANDLE)
    {
        closeConn(m_conn, flag);
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

    writeConn(m_conn, WriteWebSocketText, message.data(), (int)message.size());
}

void JZWebSocket::sendBinaryMessage(const JZByteArray &message)
{
    if (m_conn == INVALID_NET_HANDLE)
        return;

    writeConn(m_conn, WriteWebSocketBin, message.data(), message.size());
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
    }
    else if (event->event == JZ_NET_EV_WS_MSG)
    {        
        JZWsResData *ws = (JZWsResData*)(event->extData.get());
        int op = ws->wsFlag & 15;
        if (op == JZ_WEBSOCKET_OP_TEXT) {            
            sigTextMessageReceived.emit(str_fromUtf8(event->binData));
        }
        else if (op == JZ_WEBSOCKET_OP_BINARY) {
            sigBinaryMessageReceived.emit(event->binData);
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