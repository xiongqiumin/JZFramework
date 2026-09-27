#ifndef JZ_LOCAL_SOCKET_THREAD_H_
#define JZ_LOCAL_SOCKET_THREAD_H_
#include "JZThread.h"
#include "JZObjectPointer.h"
#include "JZByteArray.h"
#include <vector>
#include <map>

class JZLocalSocketServer;
class JZLocalSocketClient;

// 本地套接字专用线程(对标 JZNetThread)
//  - 所有 JZLocalSocketServer / JZLocalSocketClient 把自身的真实套接字句柄
//    注册到本线程, 由本线程统一通过 select() 完成 IO 多路复用(20ms 轮询)。
//  - accept / recv / send 等 socket 操作都集中在本线程内完成, 对象类不再关心
//    select 细节; 数据到达或连接状态变化时, 通过事件 post 回对象所属线程触发信号。
//  - 嵌入式平台(FreeRTOS / RT-Thread)没有操作系统级套接字, 走全局变量内存队列,
//    其对象也注册到本线程统一管理生命周期, 但不参与 select 轮询。
class JZFRAMEWORK_API JZLocalSocketThread : public JZThread
{
public:
    static JZLocalSocketThread* instance();

    void addObject(JZLocalSocketServer* server);
    void addObject(JZLocalSocketClient* client);
    void removeObject(JZLocalSocketServer* server);
    void removeObject(JZLocalSocketClient* client);

    // 正常 OS: 把监听套接字 / 已接受连接的 fd 注册进 select 集合
    void registerServerListenFd(JZLocalSocketServer* server);
    void registerAcceptedFd(JZLocalSocketServer* server, JZLocalSocketClient* client, int fd);
    void closeFd(int fd);

    // 写请求统一经本线程执行(对象侧调用, 由 run 循环真正发送)
    void postWrite(JZLocalSocketClient* client, const JZByteArray& data);

protected:
    struct Conn
    {
        JZObjectPointer owner;            // 事件投递目标(JZLocalSocketServer 或 Client)
        bool isListen = false;            // true=server 监听套接字(accept)
        JZLocalSocketServer* server = nullptr;
        JZLocalSocketClient* client = nullptr;   // 已接受连接对应的客户端对象
    };

    struct PendingWrite
    {
        JZObjectPointer client;           // 目标 client 对象
        JZByteArray data;
    };

    JZLocalSocketThread(JZObject* parent);
    ~JZLocalSocketThread();

    virtual void run() override;
    void pollSockets();
    void processWrites();                  // 在 run 循环内真正执行发送
    void doWrite(JZLocalSocketClient* client, const JZByteArray& data);  // 在线程上下文执行真正的发送

    std::vector<JZLocalSocketServer*> m_servers;
    std::vector<JZLocalSocketClient*> m_clients;
    std::map<int, Conn> m_fdMap;         // fd -> 连接信息
    std::vector<PendingWrite> m_writes;  // 待发送的写请求队列
    JZMutex m_mutex;
};

#endif
