#include <chrono>
#include <map>
#include <string>
#include <vector>

#include "JZDebug.h"
#include "JZTimer.h"
#include "JZEventLoop.h"
#include "JZThread.h"
#include "JZByteArray.h"

#include "JZTcpServer.h"
#include "JZTcpSocket.h"

#include "net_test_common.h"

using namespace std;

// ===================== 单线程 TCP 回环测试 =====================
class TcpTestSingle : public JZObject
{
public:
    TcpTestSingle(JZEventLoop *loop, int port)
    {
        m_prefix = "tcp";
        if(g_option.enableServer)
        {
            m_server = new JZTcpServer(this);
            m_server->listen(port);
            connect(&m_server->sigAccept, this, &TcpTestSingle::onAccept);
        }

        if(g_option.enableClient)
        {
            m_client = new JZTcpSocket(this);
            connect(&m_client->sigConnect, this, &TcpTestSingle::onClientConnect);
            connect(&m_client->sigReadyRead, this, &TcpTestSingle::onClientRead);
            connect(&m_client->sigError, this, &TcpTestSingle::onError);
            m_client->connectToHost(g_option.host, port);
        }

        m_tps.begin();
        if (g_option.enableServer && !g_option.enableClient)   // 纯服务器模式: 等30s连接, 连接后跑20s
            m_bench = serverInstallExit(this, loop, 30000, 20000);
        else
            netInstallExit(this, loop, g_option.loopTime);
        jzDebug() << "==== TCP 单线程测试启动 (" << (g_option.enableServer && !g_option.enableClient ? string("连接后20s") : to_string(g_option.loopTime / 1000) + "s") << " 后退出) ====";
    }

    void onAccept()
    {
        m_serverSock = m_server->nextPendingConnection();
        if (m_serverSock)
        {
            if (m_bench)
                m_bench->onConnected();
            connect(&m_serverSock->sigReadyRead, this, &TcpTestSingle::onServerRead);
        }
    }

    void onServerRead()
    {
        m_srvBuf += str_fromUtf8(m_serverSock->readAll());
        size_t pos;
        while ((pos = m_srvBuf.find('\n')) != string::npos)
        {
            string line = m_srvBuf.substr(0, pos);
            m_srvBuf.erase(0, pos + 1);
            m_serverSock->write(str_toUtf8(line + "\n"));   // 原样回传
        }
        if (g_option.enableServer && !g_option.enableClient)   // 纯服务器模式: 在接收时计时
            m_tps.hit();
    }

    void onClientConnect() { sendNext(); }

    void onClientRead()
    {
        m_cliBuf += str_fromUtf8(m_client->readAll());
        size_t pos;
        while ((pos = m_cliBuf.find('\n')) != string::npos)
        {
            string line = m_cliBuf.substr(0, pos);
            m_cliBuf.erase(0, pos + 1);

            uint64_t seq;
            if (!netParseMsg(line, m_prefix, seq) || seq != m_expect)
                netReportError("TCP", "校验失败 期望=" + to_string(m_expect) + " 收到=" + line);
            m_expect++;
            if (g_option.enableClient)
                m_tps.hit();
            sendNext();
        }
    }

    void onError(const string &) { netReportError("TCP", "socket 错误"); }

    void sendNext() { m_client->write(str_toUtf8(netMakeMsg(m_prefix, m_expect))); }

    JZTcpServer *m_server = nullptr;
    JZTcpSocket *m_serverSock = nullptr;
    JZTcpSocket *m_client = nullptr;
    string m_srvBuf, m_cliBuf;
    string m_prefix = "tcp";
    uint64_t m_expect = 0;
    NetTpsCounter m_tps{"TCP 单线程"};
    NetServerBench *m_bench = nullptr;
};

// ===================== 多线程客户端线程(各自独立连接) =====================
class TcpClientThread : public JZThread
{
public:
    TcpClientThread(const string &host, int port, NetTpsCounter *tps)
        : m_host(host), m_port(port), m_tps(tps) 
    {
        moveToThread(this);
        connect(&sigFinished, this, &TcpClientThread::onFinished);
    }

    void onFinished()
    {
        moveToThread(m_parent->thread());
    }

    void run() override
    {
        m_prefix = "tcp";
        m_sock = new JZTcpSocket(this);
        connect(&m_sock->sigConnect, this, &TcpClientThread::onConnect);
        connect(&m_sock->sigReadyRead, this, &TcpClientThread::onRead);
        connect(&m_sock->sigError, this, &TcpClientThread::onError);
        m_sock->connectToHost(m_host, m_port);
        exec();
    }

    void onConnect() { sendNext(); }

    void onRead()
    {
        m_buf += str_fromUtf8(m_sock->readAll());
        size_t pos;
        while ((pos = m_buf.find('\n')) != string::npos)
        {
            string line = m_buf.substr(0, pos);
            m_buf.erase(0, pos + 1);
            uint64_t seq;
            if (!netParseMsg(line, m_prefix, seq) || seq != m_expect)
                netReportError("TCP", "多线程校验失败 期望=" + to_string(m_expect) + " 收到=" + line);
            m_expect++;
            if (g_option.enableClient)
                m_tps->hit();
            sendNext();
        }
    }

    void onError(const string &) { netReportError("TCP", "多线程 socket 错误"); }

    void sendNext() { m_sock->write(str_toUtf8(netMakeMsg(m_prefix, m_expect))); }

    string m_host;
    int m_port;
    NetTpsCounter *m_tps;
    string m_prefix = "tcp";
    JZTcpSocket *m_sock = nullptr;
    string m_buf;
    uint64_t m_expect = 0;
};

// ===================== 主线程: 1 个 Server + 管理 N 个客户端线程 =====================
class TcpTestMulti : public JZObject
{
public:
    vector<TcpClientThread *> threads;
    NetTpsCounter m_tps{"TCP 多线程"};
    NetServerBench *m_bench = nullptr;

    TcpTestMulti(JZEventLoop *loop, int port)
    {
        if (g_option.enableServer)
        {
            m_server = new JZTcpServer(this);
            m_server->listen(port);
            connect(&m_server->sigAccept, this, &TcpTestMulti::onAccept);
        }

        if (g_option.enableClient)
        {
            for (int i = 0; i < NET_TEST_THREAD_COUNT; i++)
            {
                auto t = new TcpClientThread(g_option.host, port, &m_tps);
                t->start();
                t->setParent(this);
                threads.push_back(t);
            }
        }

        m_tps.begin();
        if (g_option.enableServer && !g_option.enableClient)   // 纯服务器模式: 等30s连接, 连接后跑20s
            m_bench = serverInstallExit(this, loop, 30000, 20000);
        else
            netInstallExit(this, loop, g_option.loopTime);
        jzDebug() << "==== TCP 多线程测试启动 (" << NET_TEST_THREAD_COUNT
                  << " 客户端, " << (g_option.enableServer && !g_option.enableClient ? string("连接后20s") : to_string(g_option.loopTime / 1000) + "s") << " 后退出) ====";
    }

    ~TcpTestMulti()
    {
        for (auto t : threads)
        {
            t->quit();
            t->wait();
            delete t;
        }
    }

    void onAccept()
    {
        auto *s = m_server->nextPendingConnection();
        if (s)
        {
            if (m_bench)
                m_bench->onConnected();
            connect(&s->sigReadyRead, this, &TcpTestMulti::onServerRead);
        }
    }

    void onServerRead()
    {
        auto *sock = (JZTcpSocket *)sender();
        m_bufMap[sock] += str_fromUtf8(sock->readAll());
        string &buf = m_bufMap[sock];
        size_t pos;
        while ((pos = buf.find('\n')) != string::npos)
        {
            string line = buf.substr(0, pos);
            buf.erase(0, pos + 1);
            sock->write(str_toUtf8(line + "\n"));   // 原样回传
        }
        if (g_option.enableServer && !g_option.enableClient)   // 纯服务器模式: 在接收时计时
            m_tps.hit();
    }

    JZTcpServer *m_server;
    map<JZTcpSocket *, string> m_bufMap;
};

int test_tcp_single()
{
    g_netTestActive = true;
    JZEventLoop loop;
    TcpTestSingle test(&loop, 18100);
    loop.exec();
    double tps = test.m_tps.average();
    netCheckTps("TCP 单线程", tps);
    g_netTestActive = false;
    JZThread::msleep(g_option.delayTime);
    return (int)tps;
}

int test_tcp_multi()
{
    g_netTestActive = true;
    JZEventLoop loop;
    TcpTestMulti test(&loop, 18105);
    loop.exec();
    double tps = test.m_tps.average();
    netCheckTps("TCP 多线程", tps);
    g_netTestActive = false;
    JZThread::msleep(g_option.delayTime);
    return (int)tps;
}

int test_tcp()
{
    jzDebug() << "########## TCP 测试 (单线程 + 多线程, 各 15s) ##########";
    test_tcp_single();
    jzDebug() << "【OK】TCP 单线程通过";
    test_tcp_multi();
    jzDebug() << "【OK】TCP 多线程通过";
    return 0;
}
