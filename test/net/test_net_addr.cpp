// 连接地址的可得时机:
//   服务端: accept 出来对象在 sigAccept 回调里就已有 peerIp()/peerPort()(地址由 onAccept 直接写入)
//   客户端: 地址作为 CONNECT 事件的额外信息带来, 回调前已落到对象上 ⇒ sigConnected 里已有 peerIp()
// 交叉验证: 服务端看到的 peerPort == 客户端 localPort()。

#include <atomic>
#include <cstdio>
#include <string>

#include "JZApplication.h"
#include "JZEventLoop.h"
#include "JZTimer.h"
#include "JZDebug.h"
#include "JZTcpServer.h"
#include "JZTcpSocket.h"

using namespace std;

#define LOG(...) do { printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

static atomic<bool> g_active{true};

static void fail(const string &where, const string &detail)
{
    if (!g_active.load())
        return;
    jzDebug() << "[FATAL][" << where << "] " << detail;
    exit(1);
}

class AddrTest : public JZObject
{
public:
    AddrTest(JZEventLoop *loop)
        : m_loop(loop)
    {
        m_port = 18240;

        m_server = new JZTcpServer(this);
        if (!m_server->listen("127.0.0.1", m_port))
            fail("SETUP", "监听失败");
        connect(&m_server->sigAccept, this, &AddrTest::onAccept);

        m_client = new JZTcpSocket(this);
        connect(&m_client->sigConnected, this, &AddrTest::onConnected);
        connect(&m_client->sigError, this, [](const string &err) {
            fail("CLIENT", "连接出错: " + err);
        });

        m_timer = new JZTimer(this);
        m_timer->setSingleShot(true);
        connect(&m_timer->sigTimeout, this, [this]() {
            fail("TIMEOUT", "等 accept/connect 回调超时");
            m_loop->quit();
        });
        m_timer->start(10 * 1000);

        m_client->connectToHost("127.0.0.1", m_port);
    }

    void onAccept()
    {
        JZTcpSocket *s = m_server->nextPendingConnection();
        if (!s)
            fail("ACCEPT", "nextPendingConnection 为空");

        LOG("[test] server sigAccept: peer=%s:%d local=%s:%d", s->peerIp().c_str(), s->peerPort(),
            s->localIp().c_str(), s->localPort());

        // 就在 sigAccept 回调里 —— 地址已经写好
        if (s->peerIp() != "127.0.0.1")
            fail("ACCEPT", "peerIp 期望=127.0.0.1 实际='" + s->peerIp() + "'");
        if (s->peerPort() <= 0)
            fail("ACCEPT", "peerPort 应 > 0, 实际=" + to_string(s->peerPort()));
        if (s->localPort() != m_port)
            fail("ACCEPT", "localPort 期望=" + to_string(m_port) + " 实际=" + to_string(s->localPort()));

        m_serverPeerPort = s->peerPort();
        stepDone();
    }

    void onConnected()
    {
        LOG("[test] client sigConnected: peer=%s:%d local=%s:%d", m_client->peerIp().c_str(),
            m_client->peerPort(), m_client->localIp().c_str(), m_client->localPort());

        if (m_client->peerIp() != "127.0.0.1")
            fail("CONNECT", "peerIp 期望=127.0.0.1 实际='" + m_client->peerIp() + "'");
        if (m_client->peerPort() != m_port)
            fail("CONNECT", "peerPort 期望=" + to_string(m_port) + " 实际=" + to_string(m_client->peerPort()));
        if (m_client->localPort() <= 0)
            fail("CONNECT", "localPort 应 > 0, 实际=" + to_string(m_client->localPort()));

        m_clientLocalPort = m_client->localPort();
        stepDone();
    }

    void stepDone()
    {
        if (++m_done < 2)
            return;

        // 两端看到的端口应当对上
        if (m_serverPeerPort != m_clientLocalPort)
            fail("CROSS", "服务端 peerPort=" + to_string(m_serverPeerPort) +
                              " 与客户端 localPort=" + to_string(m_clientLocalPort) + " 不一致");

        LOG("[test] accept / connect 两端地址都就位, 端口交叉核对通过 (%d)", m_serverPeerPort);
        g_active = false;
        m_loop->quit();
    }

    JZEventLoop *m_loop;
    int m_port = 0;
    JZTcpServer *m_server = nullptr;
    JZTcpSocket *m_client = nullptr;
    JZTimer *m_timer = nullptr;
    int m_serverPeerPort = 0;
    int m_clientLocalPort = 0;
    int m_done = 0;
};

// 作为 test_net 的一个步骤运行(见 test_net.cpp)
int test_netAddr()
{
    LOG("########## 连接地址测试 (accept 回调 / connect 回调 / 端口交叉核对) ##########");
    g_active = true;
    JZEventLoop loop;
    AddrTest test(&loop);
    loop.exec();
    return 0;
}
