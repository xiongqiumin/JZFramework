#ifndef JZCO_UDP_SOCKET_H_
#define JZCO_UDP_SOCKET_H_

#include <vector>
#include <string>
#include "JZCo.h"
#include "JZUdpSocket.h"
#include "JZResult.h"

// 单个 UDP 数据报（保留数据报边界与发送方地址）
struct JZCoUdpDatagram
{
    std::string peerIp;
    int peerPort;
    JZByteArray data;
};

// 协程 UDP 套接字: 包装新的 JZUdpSocket (无连接的纯数据报 API, 类似 QUdpSocket)
//   - bind(port): 绑定本地端口, 接收任意对端的数据报
//   - writeDatagram(ip, port, ...): 发送到指定对端
//   - receive(): 协程等待并读取一个数据报(含发送方地址)
//   UDP 没有连接概念, 故不提供 connectToHost / disconnectFromHost / abort / 连接态 write
class JZCoUdpSocket : public JZObject
{
public:
    JZCoUdpSocket(JZObject *parent = nullptr);
    ~JZCoUdpSocket();

    // 绑定本地端口（接收任意对端的数据报，UDP 服务端模式），同步返回
    bool bind(int port);
    void close();

    SocketState state() const;
    bool isConnected() const;

    // 读取单个数据报（保留数据报边界，含发送方地址）
    JZResult<JZCoUdpDatagram> receive(int ms = -1);

    // 发送到指定目的地址（UDP 数据报模式, 无连接概念）
    void writeDatagram(const std::string &ip, int port, const JZByteArray &buffer);
    void writeDatagram(const std::string &ip, int port, const char *buffer, int len);

    bool hasPendingDatagrams() const;

protected:
    enum WaitType
    {
        WaitNone,
        WaitRead,
    };

    void connectSignals();
    void yield(WaitType type, int timeout = -1);
    bool waitForReadyRead(int timeout);

    void onReadyRead();
    void onError(const std::string &error);

    JZUdpSocket *m_socket;
    WaitType m_waitType;
    JZCoCondition m_coCtrl;
    std::vector<JZCoUdpDatagram> m_recvQueue;
    std::string m_lastError;
};

#endif // !JZCO_UDP_SOCKET_H_
