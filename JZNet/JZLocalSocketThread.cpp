#include "JZLocalSocketThread.h"
#include "JZLocalSocketServer.h"
#include "JZLocalSocketClient.h"
#include "JZApplication.h"

#include <cstring>

#if (JZ_OS == JZ_OS_WINDOWS || JZ_OS == JZ_OS_LINUX)
#  if (JZ_OS == JZ_OS_WINDOWS)
#    include <winsock2.h>
#    include <ws2tcpip.h>
#    pragma comment(lib, "ws2_32.lib")
#    define JZ_LOCAL_INVALID_SOCKET INVALID_SOCKET
#    define JZ_LOCAL_SOCKET_ERROR SOCKET_ERROR
#    define jz_local_close closesocket
#  else
#    include <sys/select.h>
#    include <unistd.h>
#    include <errno.h>
#    define JZ_LOCAL_INVALID_SOCKET (-1)
#    define JZ_LOCAL_SOCKET_ERROR (-1)
#    define jz_local_close ::close
#  endif
#endif

// ---------------------------------------------------------------------------
static JZLocalSocketThread* g_localSocketThread = nullptr;

JZLocalSocketThread* JZLocalSocketThread::instance()
{
    if (!g_localSocketThread)
        g_localSocketThread = new JZLocalSocketThread(jzApp);
    return g_localSocketThread;
}

JZLocalSocketThread::JZLocalSocketThread(JZObject* parent)
    : JZThread(parent)
{
    JZThreadConfig cfg;
    cfg.stackSize = 8 * 1024;
    cfg.priority = 5;

    moveToThread(this);
    setObjectName("JZLocalSocketThread");
    setConfig(cfg);
}

JZLocalSocketThread::~JZLocalSocketThread()
{
    quit();
    wait();
    g_localSocketThread = nullptr;
}

void JZLocalSocketThread::addObject(JZLocalSocketServer* server)
{
    JZMutexLocker locker(&m_mutex);
    if (!vec_contains(m_servers, server))
        m_servers.push_back(server);
    if (m_servers.size() + m_clients.size() == 1 && !isRunning())
        start();
}

void JZLocalSocketThread::addObject(JZLocalSocketClient* client)
{
    JZMutexLocker locker(&m_mutex);
    if (!vec_contains(m_clients, client))
        m_clients.push_back(client);
    if (m_servers.size() + m_clients.size() == 1 && !isRunning())
        start();
}

void JZLocalSocketThread::removeObject(JZLocalSocketServer* server)
{
    JZMutexLocker locker(&m_mutex);
    for (auto it = m_fdMap.begin(); it != m_fdMap.end(); )
    {
        if (it->second.server == server)
            it = m_fdMap.erase(it);
        else
            ++it;
    }
    vec_removeAll(m_servers, server);
}

void JZLocalSocketThread::removeObject(JZLocalSocketClient* client)
{
    JZMutexLocker locker(&m_mutex);
    for (auto it = m_fdMap.begin(); it != m_fdMap.end(); )
    {
        if (it->second.client == client || it->second.owner.object() == client)
            it = m_fdMap.erase(it);
        else
            ++it;
    }
    vec_removeAll(m_clients, client);
}

void JZLocalSocketThread::registerServerListenFd(JZLocalSocketServer* server)
{
    JZMutexLocker locker(&m_mutex);
    int fd = server->listenSocketFd();
    if (fd < 0)
        return;
    Conn c;
    c.owner = server;
    c.isListen = true;
    c.server = server;
    m_fdMap[fd] = c;
}

void JZLocalSocketThread::registerAcceptedFd(JZLocalSocketServer* server,
                                            JZLocalSocketClient* client, int fd)
{
    JZMutexLocker locker(&m_mutex);
    Conn c;
    c.owner = client;          // 数据直接投递给 client 对象(对齐 JZTcpSocket)
    c.isListen = false;
    c.server = server;
    c.client = client;
    m_fdMap[fd] = c;
}

void JZLocalSocketThread::closeFd(int fd)
{
    JZMutexLocker locker(&m_mutex);
    auto it = m_fdMap.find(fd);
    if (it != m_fdMap.end())
        m_fdMap.erase(it);
#if (JZ_OS == JZ_OS_WINDOWS || JZ_OS == JZ_OS_LINUX)
    if (fd >= 0)
        jz_local_close(fd);
#endif
}

void JZLocalSocketThread::postWrite(JZLocalSocketClient* client, const JZByteArray& data)
{
    if (!client)
        return;
    {
        JZMutexLocker locker(&m_mutex);
        PendingWrite w;
        w.client = client;
        w.data = data;
        m_writes.push_back(std::move(w));
    }
    wakeUp();   // 唤醒 run 循环立即发送
}

void JZLocalSocketThread::pollSockets()
{
#if (JZ_OS == JZ_OS_WINDOWS || JZ_OS == JZ_OS_LINUX)
    fd_set read_fds;
    FD_ZERO(&read_fds);
    int maxfd = -1;

    {
        JZMutexLocker locker(&m_mutex);
        // 监听套接字
        for (auto* s : m_servers)
        {
            int fd = s->listenSocketFd();
            if (fd >= 0)
            {
                FD_SET(fd, &read_fds);
                if (fd > maxfd)
                    maxfd = fd;
                // 确保已登记(防止 listen 后未显式 register)
                if (m_fdMap.find(fd) == m_fdMap.end())
                {
                    Conn c; c.owner = s; c.isListen = true; c.server = s;
                    m_fdMap[fd] = c;
                }
            }
        }
        // 主动连接的客户端
        for (auto* c : m_clients)
        {
            int fd = c->socketFd();
            if (fd >= 0)
            {
                FD_SET(fd, &read_fds);
                if (fd > maxfd)
                    maxfd = fd;
                if (m_fdMap.find(fd) == m_fdMap.end())
                {
                    Conn cc; cc.owner = c;
                    m_fdMap[fd] = cc;
                }
            }
        }
    }

    if (maxfd < 0)
        return;

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 20000;   // 20ms 轮询
    int ret = ::select(maxfd + 1, &read_fds, nullptr, nullptr, &tv);
    if (ret <= 0)
        return;

    // 收集可读 fd 列表, 释放锁后再处理
    std::vector<int> ready;
    {
        JZMutexLocker locker(&m_mutex);
        for (auto& kv : m_fdMap)
        {
            int fd = kv.first;
            if (FD_ISSET(fd, &read_fds))
                ready.push_back(fd);
        }
    }

    for (int fd : ready)
    {
        Conn conn;
        {
            JZMutexLocker locker(&m_mutex);
            auto it = m_fdMap.find(fd);
            if (it == m_fdMap.end())
                continue;
            conn = it->second;
        }

        if (conn.isListen)
        {
            // 接受新连接
            int newfd = conn.server->acceptNewClient();
            if (newfd >= 0)
            {
                JZLocalSocketClient* client = conn.server->createAcceptedClient(newfd);
                registerAcceptedFd(conn.server, client, newfd);
                client->markConnected();
                JZApplication::instance()->postEvent(
                    conn.server, new JZLocalSocketNotifyEvent(conn.server, true));
            }
            continue;
        }

        // 读取数据
        JZObject* target = conn.owner.object();
        char buf[4096];
        int n = ::recv(fd, buf, sizeof(buf), 0);
        if (n > 0)
        {
            std::string data(buf, n);
            JZApplication::instance()->postEvent(
                target, new JZLocalSocketDataEvent(target, data, false));
        }
        else if (n == 0)
        {
            // 对端关闭
            closeFd(fd);
            if (conn.client)
                conn.server->removeAcceptedClient(conn.client);
            JZApplication::instance()->postEvent(
                target, new JZLocalSocketNotifyEvent(target, false));
        }
        else
        {
#if (JZ_OS == JZ_OS_LINUX)
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
#else
            if (::WSAGetLastError() == WSAEWOULDBLOCK)
                continue;
#endif
            closeFd(fd);
            if (conn.client)
                conn.server->removeAcceptedClient(conn.client);
            JZApplication::instance()->postEvent(
                target, new JZLocalSocketNotifyEvent(target, false));
        }
    }
#else
    // 嵌入式平台: 没有操作系统级套接字, 不进行 select 轮询
    JZThread::msleep(20);
#endif
}

void JZLocalSocketThread::processWrites()
{
    std::vector<PendingWrite> batch;
    {
        JZMutexLocker locker(&m_mutex);
        if (m_writes.empty())
            return;
        batch.swap(m_writes);
    }

    for (auto& w : batch)
    {
        JZLocalSocketClient* client = static_cast<JZLocalSocketClient*>(w.client.object());
        if (!client)
            continue;
        doWrite(client, w.data);   // 在线程上下文执行真正的发送
    }
}

// 在 JZLocalSocketThread 线程上下文执行, 完成真正的套接字发送
void JZLocalSocketThread::doWrite(JZLocalSocketClient* client, const JZByteArray& data)
{
#if (JZ_OS == JZ_OS_WINDOWS || JZ_OS == JZ_OS_LINUX)
    int sock = client->socketFd();
    if (sock == JZ_LOCAL_INVALID_SOCKET)
        return;
    int len = data.size();
    int sent = 0;
    while (sent < len)
    {
        int n = ::send(sock, data.constData() + sent, len - sent, 0);
        if (n == JZ_LOCAL_SOCKET_ERROR)
            break;
        sent += n;
    }
#else
    if (client->isConnect())
    {
        JZLocalSocketServer* server = JZLocalSocketServer::findServer(client->name());
        if (server)
            server->deliverToClient(client, data.constData(), data.size(), true);
    }
#endif
}

void JZLocalSocketThread::run()
{
    while (m_running)
    {
        processWrites();
        pollSockets();
        processEvents();
    }
}
