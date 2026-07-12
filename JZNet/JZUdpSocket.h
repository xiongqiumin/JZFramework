#ifndef JZ_UDP_SOCKET_H_
#define JZ_UDP_SOCKET_H_

#include <string>
#include "JZNetObject.h"
#include "JZNetPacket.h"

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

    // Bind to a local port for receiving from any peer (UDP server mode)
    bool bind(int port);
    // Connect to a remote host, setting default send/recv destination (UDP client mode)
    void connectToHost(std::string ip, int port);
    bool waitForConnected(int ms = 30 * 1000);
    void disconnectFromHost();

    void abort();
    void close();

    int read(char* buffer, int len);
    JZByteArray readAll();
    bool waitForReadyRead(int ms);

    // Write to the connected/bound default destination
    void write(const JZByteArray &buffer);
    void write(const char *buffer, int len);
    // Write to a specific destination (for bound mode)
    void writeDatagram(const std::string &ip, int port, const JZByteArray &buffer);
    void writeDatagram(const std::string &ip, int port, const char *buffer, int len);

    // Sender of the last received datagram
    std::string peerAddress() const;
    int peerPort() const;

    bool hasPendingDatagrams() const;

jz_signals:
    JZSigVoid sigConnect;
    JZSigVoid sigDisconnect;
    JZSigVoid sigReadyRead;
    JZSigString sigError;

protected:
    void onNetEvent(JZNetResEvent *event) override;
    void closeInternal(bool is_error);

    JZByteArray m_buffer;

    uint64_t m_client;
    SocketState m_state;
    std::string m_peerAddr;
    int m_peerPort;
};

#endif
