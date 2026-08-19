#include "JZLocalSocketServer.h"
#include "JZLocalSocketClient.h"
#include "JZLocalSocketThread.h"
#include "JZApplication.h"
#include "JZLogger.h"
#include "JZUrl.h"

#include <cstring>
#include <string>
#include <unordered_map>
#include <cstddef>
#include <mutex>
#include <queue>

#if (JZ_OS == JZ_OS_WINDOWS || JZ_OS == JZ_OS_LINUX)
#  if (JZ_OS == JZ_OS_WINDOWS)
#    include <winsock2.h>
#    include <ws2tcpip.h>
#    pragma comment(lib, "ws2_32.lib")
#    define JZ_LOCAL_INVALID_SOCKET INVALID_SOCKET
#    define JZ_LOCAL_SOCKET_ERROR SOCKET_ERROR
#  else
#    include <sys/socket.h>
#    include <sys/un.h>
#    include <netinet/in.h>
#    include <arpa/inet.h>
#    include <unistd.h>
#    include <fcntl.h>
#    define JZ_LOCAL_INVALID_SOCKET (-1)
#    define JZ_LOCAL_SOCKET_ERROR (-1)
#  endif
#else
// 嵌入式: 无操作系统级套接字, 通过全局 g_serverMap 被同进程客户端访问
#endif

// ===========================================================================
// 全局服务注册表(嵌入式平台客户端通过此访问同进程服务端; 正常OS下为空)
// ===========================================================================
std::unordered_map<std::string, JZLocalSocketServer*> JZLocalSocketServer::g_serverMap;

JZLocalSocketServer* JZLocalSocketServer::findServer(const std::string& name)
{
    auto it = g_serverMap.find(name);
    if (it == g_serverMap.end())
        return nullptr;
    return it->second;
}

void JZLocalSocketServer::registerServer(const std::string& name, JZLocalSocketServer* server)
{
    g_serverMap[name] = server;
}

void JZLocalSocketServer::unregisterServer(const std::string& name)
{
    auto it = g_serverMap.find(name);
    if (it == g_serverMap.end())
        return;
    g_serverMap.erase(it);
}

// ===========================================================================
// JZLocalSocketServer
// ===========================================================================
JZLocalSocketServer::JZLocalSocketServer(JZObject* parent)
    : JZObject(parent)
{
    JZLocalSocketThread::instance()->addObject(this);
}

JZLocalSocketServer::~JZLocalSocketServer()
{
    close();
    JZLocalSocketThread::instance()->removeObject(this);
    unregisterServer(m_name);
}

void JZLocalSocketServer::listen(const std::string& name)
{
    close();
    m_name = name;

#if (JZ_OS == JZ_OS_WINDOWS || JZ_OS == JZ_OS_LINUX)
    m_listenSock = JZ_LOCAL_INVALID_SOCKET;

#  if (JZ_OS == JZ_OS_LINUX)
    if (!name.empty() && name[0] == '/')
    {
        // 路径套接字: 先清理上次残留
        ::unlink(name.c_str());
        m_listenSock = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (m_listenSock == JZ_LOCAL_INVALID_SOCKET)
        {
            sigError.emit();
            return;
        }
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, name.c_str(), sizeof(addr.sun_path) - 1);
        if (::bind(m_listenSock, (struct sockaddr*)&addr, sizeof(addr)) == JZ_LOCAL_SOCKET_ERROR)
        {
            close();
            sigError.emit();
            return;
        }
    }
    else
#  endif
    {
        // Linux 抽象套接字(以 '\0' 开头) 或 Windows 本地 TCP
        std::string host = "127.0.0.1";
        int port = 0;
#  if (JZ_OS == JZ_OS_WINDOWS)
        // 形如 "127.0.0.1:12345"
        auto pos = name.find(':');
        if (pos != std::string::npos)
        {
            host = name.substr(0, pos);
            port = std::stoi(name.substr(pos + 1));
        }
#  else
        // 抽象套接字名(去掉前导 '/'), 使用本地回环端口
        port = 0;
#  endif

#  if (JZ_OS == JZ_OS_LINUX)
        m_listenSock = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (m_listenSock == JZ_LOCAL_INVALID_SOCKET)
        {
            sigError.emit();
            return;
        }
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        std::string abs = std::string("\0") + name;
        memcpy(addr.sun_path, abs.c_str(), abs.size());
        if (::bind(m_listenSock, (struct sockaddr*)&addr,
                   offsetof(struct sockaddr_un, sun_path) + abs.size()) == JZ_LOCAL_SOCKET_ERROR)
        {
            close();
            sigError.emit();
            return;
        }
#  else
        m_listenSock = ::socket(AF_INET, SOCK_STREAM, 0);
        if (m_listenSock == JZ_LOCAL_INVALID_SOCKET)
        {
            sigError.emit();
            return;
        }
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons((uint16_t)port);
        if (::bind(m_listenSock, (struct sockaddr*)&addr, sizeof(addr)) == JZ_LOCAL_SOCKET_ERROR)
        {
            close();
            sigError.emit();
            return;
        }
#  endif
    }

    if (::listen(m_listenSock, 8) == JZ_LOCAL_SOCKET_ERROR)
    {
        close();
        sigError.emit();
        return;
    }

#  if (JZ_OS == JZ_OS_LINUX)
    int flags = ::fcntl(m_listenSock, F_GETFL, 0);
    ::fcntl(m_listenSock, F_SETFL, flags | O_NONBLOCK);
#  else
    u_long mode = 1;
    ::ioctlsocket(m_listenSock, FIONBIO, &mode);
#  endif

    // 把监听 fd 注册进 JZLocalSocketThread 的 select 集合
    JZLocalSocketThread::instance()->registerServerListenFd(this);
    sigAccept.emit();
#else
    // 嵌入式: 注册到全局变量, 等待同进程客户端连接
    registerServer(m_name, this);
    sigAccept.emit();
#endif
}

void JZLocalSocketServer::close()
{
#if (JZ_OS == JZ_OS_WINDOWS || JZ_OS == JZ_OS_LINUX)
    if (m_listenSock != JZ_LOCAL_INVALID_SOCKET)
    {
        JZLocalSocketThread::instance()->closeFd(m_listenSock);
        m_listenSock = JZ_LOCAL_INVALID_SOCKET;
    }
    for (auto& kv : m_conns)
    {
        int fd = kv.first;
        JZLocalSocketThread::instance()->closeFd(fd);
    }
    m_conns.clear();
    m_pending.clear();
#else
    for (auto& kv : m_clients)
        detachClient(kv.second);
    m_clients.clear();
    m_incoming.clear();
#endif
}

JZLocalSocketClient* JZLocalSocketServer::nextPendingConnection()
{
    if (m_pending.empty())
        return nullptr;
    JZLocalSocketClient* client = m_pending.front();
    m_pending.pop_front();
    return client;
}

void JZLocalSocketServer::customEvent(JZEvent* event)
{
    // 服务端自身不再处理数据/连接状态事件(那些已按连接投递给各自的 client 对象)。
    // user+2 仅在嵌入式 detach 等极少数场景可能被使用, 这里统一交回基类。
    JZObject::customEvent(event);
}

// ---------------------------------------------------------------------------
// 供 JZLocalSocketThread 调用(在线程上下文)
// ---------------------------------------------------------------------------
#if (JZ_OS == JZ_OS_WINDOWS || JZ_OS == JZ_OS_LINUX)
int JZLocalSocketServer::acceptNewClient()
{
    int newfd = ::accept(m_listenSock, nullptr, nullptr);
    if (newfd == JZ_LOCAL_INVALID_SOCKET)
        return JZ_LOCAL_INVALID_SOCKET;
#  if (JZ_OS == JZ_OS_LINUX)
    int flags = ::fcntl(newfd, F_GETFL, 0);
    ::fcntl(newfd, F_SETFL, flags | O_NONBLOCK);
#  else
    u_long mode = 1;
    ::ioctlsocket(newfd, FIONBIO, &mode);
#  endif
    return newfd;
}

JZLocalSocketClient* JZLocalSocketServer::createAcceptedClient(int fd)
{
    auto* client = new JZLocalSocketClient(this);
    client->setSocketFd(fd);
    client->m_connected = true;
    m_conns[fd] = client;
    m_pending.push_back(client);   // 等待 nextPendingConnection() 取回
    return client;
}

void JZLocalSocketServer::removeAcceptedClient(JZLocalSocketClient* client)
{
    for (auto it = m_conns.begin(); it != m_conns.end(); )
    {
        if (it->second == client)
            it = m_conns.erase(it);
        else
            ++it;
    }
    for (auto it = m_pending.begin(); it != m_pending.end(); )
    {
        if (*it == client)
            it = m_pending.erase(it);
        else
            ++it;
    }
    // 连接断开后由持有方(取回者)负责释放, 这里不 delete
}
#endif

// ---------------------------------------------------------------------------
// 嵌入式: 客户端内存队列通路(始终编译; 正常OS下 m_clients/m_incoming 为空, 无副作用)
// ---------------------------------------------------------------------------
void JZLocalSocketServer::attachClient(JZLocalSocketClient* client)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    m_clients[client->m_name] = client;
    m_incoming[client];  // 初始化空队列
}

void JZLocalSocketServer::detachClient(JZLocalSocketClient* client)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    for (auto it = m_clients.begin(); it != m_clients.end(); )
    {
        if (it->second == client)
            it = m_clients.erase(it);
        else
            ++it;
    }
    m_incoming.erase(client);
}

void JZLocalSocketServer::deliverToClient(JZLocalSocketClient* client,
                                          const char* data, int len, bool binary)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    m_incoming[client].push(std::string(data, len));
    // 投递事件到客户端所属线程
    JZApplication::instance()->postEvent(
        client, new JZLocalSocketDataEvent(client, std::string(data, len), binary));
}
