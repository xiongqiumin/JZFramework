#ifndef JZ_WEB_SOCKET_H_
#define JZ_WEB_SOCKET_H_

#include "JZNetObject.h"
#include "JZByteArray.h"
#include "JZNetworkRequest.h"

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
    bool isConnect() const;

    // WSS 客户端 TLS 配置见基类 JZNetObject::setSsl(caPem, clientCertPem, clientKeyPem, skipVerify)。

    bool waitForConnected(int ms = 5000);

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

    uint64_t m_conn;
    SocketState m_state;
};

#endif // !JZ_WEB_SOCKET_H_
