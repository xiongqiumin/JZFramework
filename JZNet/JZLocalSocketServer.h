#ifndef JZ_LOCAL_SOCKET_SERVER_H_
#define JZ_LOCAL_SOCKET_SERVER_H_

#include "JZObject.h"
#include "JZSignal.h"
#include "JZByteArray.h"
#include "JZNetworkRequest.h"

#include <string>
#include <unordered_map>
#include <deque>
#include <mutex>

class JZLocalSocketClient;

// 本地套接字服务端
//  - 对外 API 没有平台差异。listen(name) 后把自身注册到 JZLocalSocketThread,
//    由其统一 select() 多路复用; accept / 数据读取都集中在线程内完成,
//    新连接 / 数据到达 / 连接状态变化后通过事件 post 回本对象线程触发信号。
//  - Linux / Windows: 通过真实本地套接字(AF_UNIX / 127.0.0.1)通信。
//  - 嵌入式(FreeRTOS / RT-Thread): 通过全局变量(全局服务注册表)被同进程内的
//    JZLocalSocketClient 直接访问, 以内存队列通信(不参与 select 轮询)。
class JZLocalSocketServer : public JZObject
{
    JZ_OBJECT_DECLARE(JZLocalSocketServer)

    friend class JZLocalSocketThread;
    friend class JZLocalSocketClient;

jz_signals:
    JZSigVoid sigAccept;
    JZSigVoid sigError;

public:
    JZLocalSocketServer(JZObject* parent = nullptr);
    ~JZLocalSocketServer();

    // 正常 OS: name 为本地套接字名(Linux: 任意字符串=抽象套接字, "/"开头=路径套接字;
    //          Windows: "host:port", 如 "127.0.0.1:12345")。
    // 嵌入式: name 为全局服务名, 客户端通过 findServer(name) 直接访问。
    void listen(const std::string& name);
    void close();

    // 取回一个已完成连接(接口对齐 JZTcpServer::nextPendingConnection)。
    // 返回的新连接对象自带 sigReadyRead / read / write(缓冲模式, 与 JZTcpSocket 一致)。
    JZLocalSocketClient* nextPendingConnection();

    bool isListening() const { return m_listenSock >= 0; }
    const std::string& serverName() const { return m_name; }

    // 嵌入式全局服务注册表
    static JZLocalSocketServer* findServer(const std::string& name);
    static void registerServer(const std::string& name, JZLocalSocketServer* server);
    static void unregisterServer(const std::string& name);

protected:
    void customEvent(JZEvent* event) override;

    // 供 JZLocalSocketThread 调用(在线程上下文, 持有 m_mutex)
    int listenSocketFd() const { return m_listenSock; }
    int acceptNewClient();                       // 执行 accept, 返回新连接 fd
    JZLocalSocketClient* createAcceptedClient(int fd);  // 创建并归属新客户端
    void removeAcceptedClient(JZLocalSocketClient* client);  // 移除已接受连接

    // 嵌入式: 客户端连接/数据投递
    void attachClient(JZLocalSocketClient* client);
    void detachClient(JZLocalSocketClient* client);
    void deliverToClient(JZLocalSocketClient* client, const char* data,
                        int len, bool binary);

private:
    std::string m_name;

    // 正常 OS 下: m_listenSock 为监听句柄, m_conns 为 fd->client 映射
    // 嵌入式下: 通过全局 g_serverMap 被客户端访问, m_clients/m_incoming 用于内存队列投递
    int m_listenSock = -1;
    std::unordered_map<int, JZLocalSocketClient*> m_conns;            // fd -> client(正常OS)
    std::deque<JZLocalSocketClient*> m_pending;                      // 待取回的新连接
    static std::unordered_map<std::string, JZLocalSocketServer*> g_serverMap;  // 嵌入式全局注册表
    std::mutex m_mtx;
    std::unordered_map<std::string, JZLocalSocketClient*> m_clients;  // name -> client(嵌入式)
    std::unordered_map<JZLocalSocketClient*, std::queue<std::string>> m_incoming;  // 嵌入式内存队列
};

#endif
