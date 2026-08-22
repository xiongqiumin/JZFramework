#ifndef JZ_LOCAL_SOCKET_CLIENT_H_
#define JZ_LOCAL_SOCKET_CLIENT_H_

#include "JZObject.h"
#include "JZSignal.h"
#include "JZByteArray.h"
#include "JZNetworkRequest.h"
#include "JZEvent.h"

class JZLocalSocketServer;
class JZLocalSocketThread;

// 跨线程投递的接收数据事件(在 JZLocalSocketThread -> 对象线程之间传递)
class JZLocalSocketDataEvent : public JZEvent
{
public:
    JZLocalSocketDataEvent(JZObject* receiver, std::string data, bool binary);
    std::string m_data;
    bool m_binary;
};

// 跨线程投递的连接状态事件(连接成功/断开)
class JZLocalSocketNotifyEvent : public JZEvent
{
public:
    JZLocalSocketNotifyEvent(JZObject* receiver, bool connected);
    bool m_connected;
};

// 本地套接字客户端(接口对齐 JZTcpSocket)
//  - 对外 API 没有平台差异。所有平台下都把自身注册到 JZLocalSocketThread,
//    由该线程统一通过 select() 完成 IO 多路复用, socket 读写集中在线程内,
//    数据到达 / 连接状态变化时通过事件 post 回本对象线程触发信号。
//  - 在 Linux / Windows 上: 通过真实的本地套接字(AF_UNIX / 127.0.0.1)通信。
//  - 在嵌入式平台(FreeRTOS / RT-Thread)上: 通过全局变量(全局服务注册表)直接访问
//    同一进程内的 JZLocalSocketServer, 以内存队列通信(不参与 select 轮询)。
class JZLocalSocketClient : public JZObject
{
    JZ_OBJECT_DECLARE(JZLocalSocketClient)

    friend class JZLocalSocketThread;
    friend class JZLocalSocketServer;

jz_signals:
    JZSigVoid sigConnect;
    JZSigVoid sigDisconnect;
    JZSigVoid sigReadyRead;
    JZSigString sigError;

public:
    JZLocalSocketClient(JZObject* parent = nullptr);
    ~JZLocalSocketClient();

    void connectToHost(const JZNetworkRequest& req);
    void disconnectFromHost();

    bool isConnected() const;
    bool waitForConnected(int ms);
    bool waitForReadyRead(int ms);

    int read(char* buffer, int len);
    JZByteArray readAll();

    void write(const JZByteArray& buffer);
    void write(const char* buffer, int len);

    const std::string& name() const { return m_name; }

protected:
    void customEvent(JZEvent* event) override;

    int socketFd() const { return m_sock; }
    void setSocketFd(int fd) { m_sock = fd; }

    // 供 JZLocalSocketThread 在 accept 后标记已连接并触发 sigConnect
    void markConnected()
    {
        m_connected = true;
        sigConnect.emit();
    }

private:
    std::string m_name;        // 服务端名(嵌入式)/连接标识(正常OS)
    bool m_connected = false;
    int m_sock = -1;           // 正常OS: socket句柄(-1 未连接); 嵌入式: -1 未连接/0 已连接占位
    JZByteArray m_buffer;      // 接收缓冲区, 由 sigReadyRead 通知读取
};

#endif
