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

class JZCoUdpSocket : public JZObject
{
public:
    JZCoUdpSocket(JZObject *parent = nullptr);
    ~JZCoUdpSocket();

    // 绑定本地端口（接收任意对端的数据报，UDP 服务端模式），同步返回
    bool bind(int port);
    // 连接远端主机，设置默认收发目的地址（UDP 客户端模式），协程等待连接结果
    bool connectToHost(std::string ip, int port, int timeout = 30 * 1000);

    void disconnectFromHost();
    void abort();
    void close();

    SocketState state() const;
    bool isConnected() const;

    // 读取单个数据报（保留数据报边界，含发送方地址）
    JZResult<JZCoUdpDatagram> receive(int ms = -1);

    // 发送到已连接的默认目的地址
    void write(const JZByteArray &buffer);
    void write(const char *buffer, int len);
    // 发送到指定目的地址（绑定模式下）
    void writeDatagram(const std::string &ip, int port, const JZByteArray &buffer);
    void writeDatagram(const std::string &ip, int port, const char *buffer, int len);

    // 最近一次接收的数据报的发送方地址
    std::string peerAddress() const;
    int peerPort() const;

    int pendingDatagramCount() const;

protected:
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
    void onError(const std::string &error);

    JZUdpSocket *m_socket;
    WaitType m_waitType;
    JZCoCondition m_coCtrl;
    std::vector<JZCoUdpDatagram> m_recvQueue;
    std::string m_lastError;
};

#endif // !JZCO_UDP_SOCKET_H_
