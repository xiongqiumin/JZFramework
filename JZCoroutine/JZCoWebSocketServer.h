#ifndef JZCO_WEB_SOCKET_SERVER_H_
#define JZCO_WEB_SOCKET_SERVER_H_

#include "JZCoCoroutine.h"
#include "JZCoWebSocket.h"
#include "JZWebSocketServer.h"

class JZCoWebSocketServer : public JZObject
{
public:
    JZCoWebSocketServer(JZObject* object = nullptr);
    ~JZCoWebSocketServer();

    bool listen(int port);
    void close();
    bool isListening() const;

    JZCoWebSocket* accept();

protected:
    void onAccept(JZWebSocket* socket);
    JZCoWebSocket* getPendingSocket();

    JZWebSocketServer* m_server;
    std::vector<JZCoWebSocket*> m_pendingList;
    JZCoCondition m_coCtrl;
};

#endif // !JZCO_NET_WORK_H
