#include <chrono>
#include <string>
#include <vector>

#include "JZDebug.h"
#include "JZTimer.h"
#include "JZEventLoop.h"
#include "JZThread.h"
#include "JZByteArray.h"

#include "JZUdpSocket.h"

#include "net_test_common.h"

using namespace std;

// ===================== 单线程 UDP 回环测试 =====================
class UdpTestSingle : public JZObject
{
public:
    UdpTestSingle(JZEventLoop *loop, int port)
    {
        m_prefix = "udp";
        m_server = new JZUdpSocket(this);
        m_server->bind(port);
        connect(&m_server->sigReadyRead, this, &UdpTestSingle::onServerRead);

        m_client = new JZUdpSocket(this);
        connect(&m_client->sigReadyRead, this, &UdpTestSingle::onClientRead);
        connect(&m_client->sigConnect, this, &UdpTestSingle::onClientConnect);
        connect(&m_client->sigError, this, &UdpTestSingle::onError);
        m_client->connectToHost("127.0.0.1", port);

        m_tps.begin();
        netInstallExit(this, loop, 15000);
        jzDebug() << "==== UDP 单线程测试启动 (15s 后退出) ====";
    }

    void onServerRead()
    {
        string peer = m_server->peerAddress();
        int pport = m_server->peerPort();
        m_srvBuf += str_fromUtf8(m_server->readAll());
        size_t pos;
        while ((pos = m_srvBuf.find('\n')) != string::npos)
        {
            string line = m_srvBuf.substr(0, pos);
            m_srvBuf.erase(0, pos + 1);
            m_server->writeDatagram(peer, pport, str_toUtf8(line + "\n"));
        }
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
            if (!netParseMsg(line, m_prefix, seq))
                netReportError("UDP", "解析失败 收到=" + line);
            // UDP 不可靠, 仅做宽松计数(按最大序号推进)
            if (seq > m_maxSeq)
            {
                m_tps.hit((int)(seq - m_maxSeq));
                m_maxSeq = seq;
            }
            sendNext();
        }
    }

    void onError(const string &) { netReportError("UDP", "socket 错误"); }

    void sendNext() { m_client->write(str_toUtf8(netMakeMsg(m_prefix, m_expect))); }

    JZUdpSocket *m_server;
    JZUdpSocket *m_client;
    string m_srvBuf, m_cliBuf;
    string m_prefix = "udp";
    uint64_t m_expect = 0;
    uint64_t m_maxSeq = 0;
    NetTpsCounter m_tps{"UDP 单线程"};
};

// ===================== 多线程客户端线程(各自独立 UDP socket) =====================
class UdpClientThread : public JZThread
{
public:
    UdpClientThread(const string &host, int port, NetTpsCounter *tps)
        : m_host(host), m_port(port), m_tps(tps) {}

    void run() override
    {
        m_prefix = "udp";
        m_sock = new JZUdpSocket(this);
        connect(&m_sock->sigReadyRead, this, &UdpClientThread::onRead);
        connect(&m_sock->sigError, this, &UdpClientThread::onError);
        m_sock->connectToHost(m_host, m_port);

        // 看门狗: UDP 可能丢包, 超时未确认则重发当前序号(幂等)
        m_watchdog = new JZTimer(this);
        connect(&m_watchdog->sigTimeout, this, &UdpClientThread::onWatchdog);
        m_watchdog->start(150);

        sendNext();
        exec();
    }

    void onRead()
    {
        m_buf += str_fromUtf8(m_sock->readAll());
        size_t pos;
        while ((pos = m_buf.find('\n')) != string::npos)
        {
            string line = m_buf.substr(0, pos);
            m_buf.erase(0, pos + 1);
            uint64_t seq;
            if (!netParseMsg(line, m_prefix, seq))
                netReportError("UDP", "多线程解析失败 收到=" + line);
            if (seq != m_expect)
                netReportError("UDP", "多线程校验失败 期望=" + to_string(m_expect) + " 收到=" + line);
            m_acked = true;
            m_expect++;
            m_tps->hit();
            sendNext();
        }
    }

    void onError(const string &) { netReportError("UDP", "多线程 socket 错误"); }

    void onWatchdog()
    {
        if (!m_acked)
            sendNext();   // 重发未确认序号(幂等: 服务器回显相同值)
    }

    void sendNext()
    {
        m_acked = false;
        m_sock->write(str_toUtf8(netMakeMsg(m_prefix, m_expect)));
    }

    string m_host;
    int m_port;
    NetTpsCounter *m_tps;
    string m_prefix = "udp";
    JZUdpSocket *m_sock = nullptr;
    JZTimer *m_watchdog = nullptr;
    string m_buf;
    uint64_t m_expect = 0;
    bool m_acked = true;
};

// ===================== 主线程: 1 个 Server + 管理 N 个客户端线程 =====================
class UdpTestMulti : public JZObject
{
public:
    vector<UdpClientThread *> threads;
    NetTpsCounter m_tps{"UDP 多线程"};

    UdpTestMulti(JZEventLoop *loop, int port)
    {
        m_server = new JZUdpSocket(this);
        m_server->bind(port);
        connect(&m_server->sigReadyRead, this, &UdpTestMulti::onServerRead);

        for (int i = 0; i < NET_TEST_THREAD_COUNT; i++)
        {
            auto t = new UdpClientThread("127.0.0.1", port, &m_tps);
            t->start();
            threads.push_back(t);
        }

        m_tps.begin();
        netInstallExit(this, loop, 15000);
        jzDebug() << "==== UDP 多线程测试启动 (" << NET_TEST_THREAD_COUNT << " 客户端, 15s 后退出) ====";
    }

    ~UdpTestMulti()
    {
        for (auto t : threads)
        {
            t->quit();
            t->wait();
        }
    }

    void onServerRead()
    {
        string peer = m_server->peerAddress();
        int pport = m_server->peerPort();
        string data = str_fromUtf8(m_server->readAll());
        size_t pos;
        while ((pos = data.find('\n')) != string::npos)
        {
            string line = data.substr(0, pos);
            data.erase(0, pos + 1);
            m_server->writeDatagram(peer, pport, str_toUtf8(line + "\n"));
        }
    }

    JZUdpSocket *m_server;
};

int test_udp_single()
{
    g_netTestActive = true;
    JZEventLoop loop;
    UdpTestSingle test(&loop, 18101);
    loop.exec();
    double tps = test.m_tps.average();
    netCheckTps("UDP 单线程", tps);
    g_netTestActive = false;
    return (int)tps;
}

int test_udp_multi()
{
    g_netTestActive = true;
    JZEventLoop loop;
    UdpTestMulti test(&loop, 18106);
    loop.exec();
    double tps = test.m_tps.average();
    netCheckTps("UDP 多线程", tps);
    g_netTestActive = false;
    return (int)tps;
}

int test_udp()
{
    jzDebug() << "########## UDP 测试 (单线程 + 多线程, 各 15s) ##########";
    test_udp_single();
    jzDebug() << "【OK】UDP 单线程通过";
    test_udp_multi();
    jzDebug() << "【OK】UDP 多线程通过";
    return 0;
}
