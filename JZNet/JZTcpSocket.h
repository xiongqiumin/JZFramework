#ifndef JZ_TCP_SOCKET_H_
#define JZ_TCP_SOCKET_H_
#include <string>
#include "JZNetObject.h"
#include "JZNetPacket.h"
#include "JZNetPacketProcessor.h"

class JZTcpServer;
class JZFRAMEWORK_API JZTcpSocket : public JZNetObject
{
    JZ_OBJECT_DECLARE(JZTcpSocket)

public:
    JZTcpSocket(JZObject *parent = nullptr);
    ~JZTcpSocket();

    SocketState state() const;
    bool isConnected() const;

    void connectToHost(std::string ip, int port);
    bool waitForConnected(int ms = 30 * 1000);
    void disconnectFromHost();

    void abort();
    void close();

    int read(char* buffer, int len);
    JZByteArray readAll();
    bool waitForReadyRead(int ms);
    
    void write(const JZByteArray &buffer);
    void write(const char *buffer,int len);
    
jz_signals:
    JZSigVoid sigConnected;
    JZSigVoid sigDisconnected;
    JZSigVoid sigReadyRead;
    JZSigString sigError;

protected:
    friend JZTcpServer;

    void onNetEvent(JZNetResEvent*event);
    void closeInternal(bool is_error);

    JZByteArray m_buffer;

    SocketState m_state;
};

#endif