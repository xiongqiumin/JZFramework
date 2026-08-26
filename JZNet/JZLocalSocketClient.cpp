#include "JZLocalSocketClient.h"
#include "JZLocalSocketServer.h"
#include "JZLocalSocketThread.h"
#include "JZApplication.h"
#include "JZUrl.h"

#include <cstring>
#include <string>
#include <thread>

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
// 嵌入式: 无操作系统级套接字, 通过全局变量访问同进程内的服务端
#endif

// ---------------------------------------------------------------------------
// 跨线程把接收到的数据/状态投递回对象所属线程
// ---------------------------------------------------------------------------
JZLocalSocketDataEvent::JZLocalSocketDataEvent(JZObject* receiver,
                                               std::string data, bool binary)
    : JZEvent(receiver, JZEvent::JZEvent_user + 1)
    , m_data(std::move(data))
    , m_binary(binary)
{
}

JZLocalSocketNotifyEvent::JZLocalSocketNotifyEvent(JZObject* receiver, bool connected)
    : JZEvent(receiver, JZEvent::JZEvent_user + 2)
    , m_connected(connected)
{
}

// ===========================================================================
// JZLocalSocketClient
// ===========================================================================
JZLocalSocketClient::JZLocalSocketClient(JZObject* parent)
    : JZObject(parent)
    , sigConnected(this)
    , sigDisconnected(this)
    , sigReadyRead(this)
    , sigError(this)
{
}

JZLocalSocketClient::~JZLocalSocketClient()
{
    disconnectFromHost();
    JZLocalSocketThread::instance()->removeObject(this);
}

void JZLocalSocketClient::connectToHost(const JZNetworkRequest& req)
{
    disconnectFromHost();
    const JZUrl& url = req.url();
    m_name = url.host();

#if (JZ_OS == JZ_OS_WINDOWS || JZ_OS == JZ_OS_LINUX)
    m_sock = JZ_LOCAL_INVALID_SOCKET;

#  if (JZ_OS == JZ_OS_LINUX)
    if (url.scheme() == "local" && url.path().size())
    {
        m_sock = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (m_sock == JZ_LOCAL_INVALID_SOCKET)
        {
            sigError.emit("socket error");
            return;
        }
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        std::string abs = std::string("\0") + url.path();
        memcpy(addr.sun_path, abs.c_str(), abs.size());
        if (::connect(m_sock, (struct sockaddr*)&addr,
                      offsetof(struct sockaddr_un, sun_path) + abs.size())
            == JZ_LOCAL_SOCKET_ERROR)
        {
            m_sock = JZ_LOCAL_INVALID_SOCKET;
            sigError.emit("connect error");
            return;
        }
    }
    else
#  endif
    {
        int port = url.port();
        std::string host = url.host();
        m_sock = ::socket(AF_INET, SOCK_STREAM, 0);
        if (m_sock == JZ_LOCAL_INVALID_SOCKET)
        {
            sigError.emit("socket error");
            return;
        }
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons((uint16_t)port);
        if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1)
        {
            m_sock = JZ_LOCAL_INVALID_SOCKET;
            sigError.emit("invalid host");
            return;
        }
        if (::connect(m_sock, (struct sockaddr*)&addr, sizeof(addr))
            == JZ_LOCAL_SOCKET_ERROR)
        {
            m_sock = JZ_LOCAL_INVALID_SOCKET;
            sigError.emit("connect error");
            return;
        }
    }

    // 设为非阻塞, 由 JZLocalSocketThread 统一 select 读取
#  if (JZ_OS == JZ_OS_LINUX)
    int flags = ::fcntl(m_sock, F_GETFL, 0);
    ::fcntl(m_sock, F_SETFL, flags | O_NONBLOCK);
#  else
    u_long mode = 1;
    ::ioctlsocket(m_sock, FIONBIO, &mode);
#  endif
    m_connected = true;
    JZLocalSocketThread::instance()->addObject(this);
    sigConnected.emit();
#else
    // 嵌入式: 通过全局变量访问同进程内的服务端
    JZLocalSocketServer* server = JZLocalSocketServer::findServer(m_name);
    if (!server)
    {
        sigError.emit("server not found");
        return;
    }
    m_sock = 0;   // 占位: 表示已连接(无真实 socket 句柄)
    m_connected = true;
    server->attachClient(this);
    JZLocalSocketThread::instance()->addObject(this);
    sigConnected.emit();
#endif
}

void JZLocalSocketClient::disconnectFromHost()
{
#if (JZ_OS == JZ_OS_WINDOWS || JZ_OS == JZ_OS_LINUX)
    if (m_sock != JZ_LOCAL_INVALID_SOCKET)
    {
        JZLocalSocketThread::instance()->closeFd(m_sock);
        m_sock = JZ_LOCAL_INVALID_SOCKET;
    }
#else
    if (m_connected)
    {
        JZLocalSocketServer* server = JZLocalSocketServer::findServer(m_name);
        if (server)
            server->detachClient(this);
    }
    m_sock = -1;
#endif
    if (m_connected)
    {
        m_connected = false;
        sigDisconnected.emit();
    }
}

bool JZLocalSocketClient::isConnected() const
{
    return m_connected;
}

bool JZLocalSocketClient::waitForConnected(int ms)
{
    int waited = 0;
    while (waited < ms)
    {
        if (isConnected())
            return true;
        JZApplication::instance()->processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        waited += 10;
    }
    return isConnected();
}

bool JZLocalSocketClient::waitForReadyRead(int ms)
{
    int waited = 0;
    while (waited < ms)
    {
        if (m_buffer.size() > 0)
            return true;
        else if (!m_connected)
            return false;
        JZApplication::instance()->processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        waited += 10;
    }
    return m_buffer.size() > 0;
}

int JZLocalSocketClient::read(char* buffer, int len)
{
    if (len <= 0)
        return 0;
    int available = (int)m_buffer.size();
    int read_size = (len < available) ? len : available;
    if (read_size > 0)
    {
        memcpy(buffer, m_buffer.data(), read_size);
        m_buffer = m_buffer.mid(read_size);
    }
    return read_size;
}

JZByteArray JZLocalSocketClient::readAll()
{
    JZByteArray buffer;
    buffer.swap(m_buffer);
    return buffer;
}

void JZLocalSocketClient::write(const JZByteArray& buffer)
{
    // 写请求统一投递到 JZLocalSocketThread, 由其 run 循环执行真正的发送
    JZLocalSocketThread::instance()->postWrite(this, buffer);
}

void JZLocalSocketClient::write(const char* buffer, int len)
{
    if (len <= 0)
        return;
    // 写请求统一投递到 JZLocalSocketThread, 由其 run 循环执行真正的发送
    JZLocalSocketThread::instance()->postWrite(this, JZByteArray(buffer, len));
}

void JZLocalSocketClient::customEvent(JZEvent* event)
{
    if (event->type() == JZEvent::JZEvent_user + 1)
    {
        auto* e = static_cast<JZLocalSocketDataEvent*>(event);
        m_buffer.append(e->m_data.data(), (int)e->m_data.size());
        sigReadyRead.emit();
    }
    else if (event->type() == JZEvent::JZEvent_user + 2)
    {
        auto* e = static_cast<JZLocalSocketNotifyEvent*>(event);
        if (e->m_connected)
            sigConnected.emit();
        else
            sigDisconnected.emit();
    }
    else
    {
        JZObject::customEvent(event);
    }
}
