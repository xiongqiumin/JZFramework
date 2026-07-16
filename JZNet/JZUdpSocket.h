#ifndef JZ_UDP_SOCKET_H_
#define JZ_UDP_SOCKET_H_

#include <string>
#include <deque>
#include "JZNetObject.h"
#include "JZNetPacket.h"

// 单个接收的数据报(含发送方地址)
struct JZUdpDatagram
{
    JZByteArray data;
    std::string peerIp;
    int peerPort = 0;
};

// UDP response data (sender address of received datagram)
class JZUdpResData : public JZNetResEventData
{
public:
    std::string peerIp;
    int peerPort;
};

// UDP write destination info (for writeDatagram to a specific peer)
class JZUdpWriteData : public JZNetReqEventData
{
public:
    std::string ip;
    int port;
};

class JZUdpSocket : public JZNetObject
{
    JZ_OBJECT_DECLARE(JZUdpSocket)

public:
    JZUdpSocket(JZObject *parent = nullptr);
    ~JZUdpSocket();

    SocketState state() const;

    // 绑定到本地端口, 接收任意对端的数据报(对应 QUdpSocket::bind)
    bool bind(int port);
    void close();

    // 发送数据报到指定目标(对应 QUdpSocket::writeDatagram)
    void writeDatagram(const std::string &ip, int port, const JZByteArray &buffer);
    void writeDatagram(const std::string &ip, int port, const char *buffer, int len);

    // 读取一个数据报(含发送方地址), 队列为空时返回空数据报(对应 QUdpSocket::readDatagram)
    JZUdpDatagram readDatagram();
    bool hasPendingDatagrams() const;

jz_signals:
    JZSigVoid sigReadyRead;
    JZSigString sigError;

protected:
    void onNetEvent(JZNetResEvent *event) override;
    void closeInternal(bool is_error);

    std::deque<JZUdpDatagram> m_datagrams;  // 按数据报逐个保存
    std::string m_peerAddr;
    int m_peerPort = 0;

    uint64_t m_client = INVALID_NET_HANDLE;
    SocketState m_state = UnconnectedState;
};

#endif
