#ifndef JZ_WEB_SOCKET_SERVER_H_
#define JZ_WEB_SOCKET_SERVER_H_

#include "JZNetObject.h"
#include "JZByteArray.h"
#include "JZNetworkRequest.h"
#include "JZWebSocket.h"

class JZWebSocketServer : public JZNetObject
{
    JZ_OBJECT_DECLARE(JZWebSocketServer)

public:
    JZWebSocketServer(JZObject *parent = nullptr);
    ~JZWebSocketServer();

    bool listen(int port);
    void close();
    bool isListening() const;

jz_signals:
    JZSignal<JZWebSocket*> sigAccept;

protected:    
    virtual JZNetObject* onAccept(uint64_t conn); 
    virtual void onNetEvent(JZNetResEvent*event) override;

    uint64_t m_listen;
};

#endif // !JZ_WEB_SOCKET_H_
