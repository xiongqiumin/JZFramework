#ifndef JZ_WEB_SOCKET_SERVER_H_
#define JZ_WEB_SOCKET_SERVER_H_

#include "JZNetObject.h"
#include "JZByteArray.h"
#include "JZNetworkRequest.h"
#include "JZWebSocket.h"
#include <deque>

class JZWebSocketServer : public JZNetObject
{
    JZ_OBJECT_DECLARE(JZWebSocketServer)

public:
    JZWebSocketServer(JZObject *parent = nullptr);
    ~JZWebSocketServer();

    bool listen(int port);
    bool listen(std::string ip, int port);
    void close();
    bool isListening() const;

    // WSS 服务端 TLS 配置见基类 JZNetObject::setSsl(caPem, certPem, keyPem)。

    JZWebSocket* nextPendingConnection();

jz_signals:
    JZSigVoid sigAccept;

protected:    
    virtual JZNetObject* onAccept(uint64_t conn); 
    virtual void onNetEvent(JZNetResEvent*event) override;

    std::deque<JZWebSocket*> m_pendingConnections;
};

#endif // !JZ_WEB_SOCKET_H_
