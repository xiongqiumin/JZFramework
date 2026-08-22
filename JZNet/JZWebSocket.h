#ifndef JZ_WEB_SOCKET_H_
#define JZ_WEB_SOCKET_H_

#include "JZNetObject.h"
#include "JZByteArray.h"
#include "JZNetworkRequest.h"
#include "JZTimer.h"

class JZWebSocketServer;

// WebSocket response data
class JZWsResData : public JZNetResEventData
{
public:
    int wsFlag;
};

class JZWebSocket : public JZNetObject
{
    JZ_OBJECT_DECLARE(JZWebSocket)

public:
    JZWebSocket(JZObject *parent = nullptr);
    ~JZWebSocket();

    SocketState state() const;

    void connectToHost(const std::string &uri);
    void connectToHost(const JZNetworkRequest &req);
    void disconnectFromHost();
    bool isConnected() const;

    bool waitForConnected(int ms = 5000);

    void setAutoPing(bool enable, int intervalMs = 60 * 1000, int timeoutMs = 15000);
    // 手动发送一帧 PING(通常无需调用, 由 setAutoPing 自动管理)
    void sendPing();

    void sendTextMessage(const std::string &message);
    void sendBinaryMessage(const JZByteArray &message);

jz_signals:
    JZSigVoid sigConnected;
    JZSigVoid sigDisconnected;
    JZSigString sigTextMessageReceived;
    JZSigByteArray sigBinaryMessageReceived;
    JZSigVoid sigError;

protected:    
    friend JZWebSocketServer;

    virtual void onNetEvent(JZNetResEvent* event) override;
    void closeInternal(bool is_error);

    // 自动 ping 相关
    void onPingTick();
    void onPongReceived();
    void startAutoPing();
    void stopAutoPing();

    SocketState m_state;

    bool m_autoPing = false;        // 是否开启自动 ping
    int m_pingIntervalMs = 15000;   // ping 间隔
    int m_pingTimeoutMs = 5000;     // 等待 pong 超时
    JZTimer m_pingTimer;            // 周期 tick(每秒) 的定时器
    int64_t m_lastPingMs = 0;       // 上次发出 ping 的时间戳
    bool m_recvPong = true;    
};

#endif // !JZ_WEB_SOCKET_H_
