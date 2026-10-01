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

    // 半关闭(对齐 Node 的 socket.end()): 只关写方向 —— 对端收到 FIN('end'), 本端仍能收数据,
    // 连接不拆(真正断开用 disconnectFromHost()/close())。
    void endWrite();

    // 真半关闭(对齐 Node 的 socket.end()): endWrite() 打标记 + 给对端**发 FIN**(shutdown SD_SEND)。
    // 读方向继续; 对端 FIN 到达时走: MG_EV_FIN_REQ → 本类 sigReadFin → JS 'end'
    // → 父类 JZNetObject::event() 发 Net_FinRes → is_draining → 发送缓冲排空后关连接
    // (见 JZNet/mongoose.c 的 iolog)。end()/shutdownWrite() 幂等。
    void shutdownWrite();

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
    // 读方向 EOF(对端发来 FIN): JS 侧据此发 'end' —— **此刻回包还能发出去**(收尾前会排空发送队列)
    JZSigVoid sigReadFin;
    JZSigString sigError;

protected:
    friend JZTcpServer;

    void onNetEvent(JZNetResEvent*event);
    void closeInternal(bool is_error);

    JZByteArray m_buffer;

    SocketState m_state;
};

#endif