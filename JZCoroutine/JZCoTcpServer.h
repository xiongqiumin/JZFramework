#ifndef JZCO_TCP_SERVER_H_
#define JZCO_TCP_SERVER_H_

#include "JZTcpServer.h"
#include "JZCoTcpSocket.h"

// ========================== 协程 TCP 服务器 ==========================
class JZCoTcpServer : public JZObject
{
    JZ_OBJECT_DECLARE(JZCoTcpServer)
public:
    JZCoTcpServer(JZObject *parent = nullptr);
    ~JZCoTcpServer();

    bool listen(int port);
    void close();
    bool isListening() const;

    // 协程等待客户端连接
    JZCoTcpSocket* accept(int timeout = -1);

protected:
    void onAccept();

protected:
    JZTcpServer *m_server;
    std::vector<JZCoTcpSocket*> m_pendingList;
    JZCoCondition m_coCtrl;
};

#endif