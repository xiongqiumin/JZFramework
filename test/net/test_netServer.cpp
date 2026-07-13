#include <string>
#include <vector>

#include "JZDebug.h"
#include "JZTimer.h"
#include "JZEventLoop.h"
#include "JZThread.h"

#include "JZNetServer.h"
#include "JZNetClient.h"
#include "JZNetPacket.h"

#include "net_test_common.h"

using namespace std;

// 确保 echo 等包类型已在 JZNetPackManager 注册
static void ensureNetPackInit()
{
    static bool inited = false;
    if (!inited)
    {
        JZNetPackInit();
        inited = true;
    }
}

// ===================== 单线程 JZNetServer 回环测试 =====================
class NetServerSingleTest : public JZObject
{
public:
    NetServerSingleTest(JZEventLoop *loop, int port)
    {
        ensureNetPackInit();

        server = new JZNetServer(this);
        server->listen(port);
        connect(&server->sigAccept, this, &NetServerSingleTest::onAccept);
        connect(&server->sigPacketReceive, this, &NetServerSingleTest::onServerRecv);
        connect(&server->sigDisconnect, this, &NetServerSingleTest::onDisconnect);

        client = new JZNetClient(this);
        client->connectToHost("127.0.0.1", port);
        if (!client->waitForConnected(3000))
            netReportError("JZNetServer", "单线程 client 连接失败");
        connect(&client->sigPacketReceive, this, &NetServerSingleTest::onClientRecv);

        m_tps.begin();
        sendNext();                       // 启动 ping-pong
        netInstallExit(this, loop, 15000);
        jzDebug() << "==== JZNetServer 单线程测试启动 (15s 后退出) ====";
    }

    void onAccept() { jzDebug() << "[JZNetServer] 单线程 accept"; }

    // Server 收到任意客户端包, 原样回传
    void onServerRecv(uint64_t id, JZNetPackPtr p) { server->sendPacket(id, p.get()); }

    void onDisconnect(uint64_t id)
    {
        netReportError("JZNetServer", "单线程 client 异常断开 id=" + to_string(id));
    }

    void onClientRecv(JZNetPackPtr p)
    {
        if (p->packetType() != NetPack_echo)
            netReportError("JZNetServer", "单线程收到非 echo 包 type=" + to_string(p->packetType()));
        m_tps.hit(1);
        sendNext();
    }

    void sendNext()
    {
        JZNetPackEcho echo;
        echo.text = "hello jznet";
        client->sendPacket(&echo);
    }

    JZNetServer *server = nullptr;
    JZNetClient *client = nullptr;
    NetTpsCounter m_tps{"JZNetServer 单线程"};
};

// ===================== 多线程客户端线程(各自独立连接) =====================
class NetServerClientThread : public JZThread
{
public:
    NetServerClientThread(const string &host, int port, NetTpsCounter *tps)
        : m_host(host), m_port(port), m_tps(tps) 
    {
        moveToThread(this);
        connect(&sigFinished, this, &NetServerClientThread::onFinished);
    }

    void onFinished()
    {
        moveToThread(m_parent->thread());
    }

    void run() override
    {
        JZNetClient *client = new JZNetClient(this);
        connect(&client->sigPacketReceive, this, [this, client](JZNetPackPtr p)
        {
            if (p->packetType() != NetPack_echo)
                netReportError("JZNetServer", "多线程收到非 echo 包 type=" + to_string(p->packetType()));
            m_tps->hit(1);
            JZNetPackEcho echo;
            echo.text = "hello jznet";
            client->sendPacket(&echo);
        });
        connect(&client->sigConnect, this, [this, client]()
        {
            JZNetPackEcho echo;
            echo.text = "hello jznet";
            client->sendPacket(&echo);
        });
        connect(&client->sigDisconnect, this, [this]()
        {
            netReportError("JZNetServer", "多线程 client 异常断开");
        });
        client->connectToHost(m_host, m_port);
        exec();
    }

    string m_host;
    int m_port;
    NetTpsCounter *m_tps;
};

// ===================== 主线程: 1 个 Server + 管理 N 个客户端线程 =====================
class NetServerMultiTest : public JZObject
{
public:
    vector<NetServerClientThread *> threads;
    NetTpsCounter m_tps{"JZNetServer 多线程"};

    NetServerMultiTest(JZEventLoop *loop, int port)
    {
        ensureNetPackInit();

        server = new JZNetServer(this);
        server->listen(port);
        connect(&server->sigPacketReceive, this, &NetServerMultiTest::onServerRecv);
        connect(&server->sigDisconnect, this, &NetServerMultiTest::onDisconnect);

        for (int i = 0; i < NET_TEST_THREAD_COUNT; i++)
        {
            auto t = new NetServerClientThread("127.0.0.1", port, &m_tps);
            t->setParent(this);
            t->start();
            threads.push_back(t);
        }

        m_tps.begin();
        netInstallExit(this, loop, 15000);
        jzDebug() << "==== JZNetServer 多线程测试启动 (" << NET_TEST_THREAD_COUNT << " 客户端, 15s 后退出) ====";
    }

    ~NetServerMultiTest()
    {
        for (auto t : threads)
        {
            t->quit();
            t->wait();
            delete t;
        }
    }

    // Server 收到任意客户端包, 原样回传给对应客户端
    void onServerRecv(uint64_t id, JZNetPackPtr p) { server->sendPacket(id, p.get()); }

    void onDisconnect(uint64_t /*id*/) {}

    JZNetServer *server = nullptr;
};

int test_netServer_single()
{
    g_netTestActive = true;
    JZEventLoop loop;
    NetServerSingleTest test(&loop, 18109);
    loop.exec();
    double tps = test.m_tps.average();
    netCheckTps("JZNetServer 单线程", tps);
    g_netTestActive = false;
    return (int)tps;
}

int test_netServer_multi()
{
    g_netTestActive = true;
    JZEventLoop loop;
    NetServerMultiTest test(&loop, 18110);
    loop.exec();
    double tps = test.m_tps.average();
    netCheckTps("JZNetServer 多线程", tps);
    g_netTestActive = false;
    return (int)tps;
}

int test_netServer()
{
    jzDebug() << "########## JZNetServer 测试 (单线程 + 多线程, 各 15s) ##########";
    test_netServer_single();
    jzDebug() << "【OK】JZNetServer 单线程通过";
    test_netServer_multi();
    jzDebug() << "【OK】JZNetServer 多线程通过";
    return 0;
}
