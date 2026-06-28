#ifndef JZCO_TCP_SOCKET_H_
#define JZCO_TCP_SOCKET_H_

#include "JZCo.h"
#include "JZTcpSocket.h"
#include "JZResult.h"

class JZCoTcpServer;
class JZCoTcpSocket : public JZObject
{
public:
    JZCoTcpSocket(JZObject *parent = nullptr);
    ~JZCoTcpSocket();

    // 直接返回连接结果（协程等待，无单独wait函数）
    bool connectToHost(std::string ip, int port, int timeout = 30 * 1000);

    void disconnectFromHost();
    SocketState state() const;
    bool isConnected() const;

    // 读取接口
    JZResult<JZByteArray> readAll(int ms = -1);
    JZResult<JZByteArray> read(int len, int ms = -1);

    // 发送
    void write(const JZByteArray &buffer);
    void write(const char *buffer, int len);

protected:
    friend JZCoTcpServer;

    enum WaitType
    {
        WaitNone,
        WaitConnect,
        WaitRead,
    };

    void connectSignals();
    void yield(WaitType type, int timeout = -1);
    bool waitForReadyRead(int timeout);

    void onConnected();
    void onDisconnected();
    void onReadyRead();

    JZTcpSocket *m_socket;
    WaitType m_waitType;
    JZCoCondition m_coCtrl;
    JZByteArray m_recvBuffer;
};

#endif