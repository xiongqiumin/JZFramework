#include <atomic>
#include <map>
#include <string>
#include <vector>

#include "JZDebug.h"
#include "JZWebSocketServer.h"
#include "JZWebSocket.h"
#include "JZTimer.h"
#include "JZEventLoop.h"
#include "JZThread.h"
#include "net_test_common.h"

using namespace std;

static const string WS_PREFIX = "hello world ";

// ===================== 单线程 WebSocket 回环测试 =====================
class JZWebSocketTest : public JZObject
{
public:
    std::atomic<uint64_t> send_seq{0};		  // 客户端发送序号
    std::atomic<uint64_t> expect_recv_seq{0}; // 期望接收的序号
    JZWebSocketServer* server = nullptr;
    JZWebSocket* client = nullptr;
    JZWebSocket* server_socket = nullptr;
    NetTpsCounter m_tps{"WebSocket 单线程"};
    NetServerBench *m_bench = nullptr;

    JZWebSocketTest(JZEventLoop *loop)
    {
        int port = 18000;
        if (g_option.enableServer)
        {
            server = new JZWebSocketServer(this);
            server->listen(port);
            connect(&server->sigAccept, this, &JZWebSocketTest::onServerAccecpt);
        }

        if (g_option.enableClient)
        {
            client = new JZWebSocket(this);
            client->connectToHost(JZNetworkRequest("ws://" + g_option.host + ":" + to_string(port)));
            connect(&client->sigConnected, this, &JZWebSocketTest::onClientConnect);
            connect(&client->sigTextMessageReceived, this, &JZWebSocketTest::onClientRecv);
        }

        m_tps.begin();
        if (g_option.enableServer && !g_option.enableClient)   // 纯服务器模式: 等30s连接, 连接后跑20s
            m_bench = serverInstallExit(this, loop, 30000, 20000);
        else
            netInstallExit(this, loop, g_option.loopTime);
        jzDebug() << "==== WebSocket 单线程测试启动 (" << (g_option.enableServer && !g_option.enableClient ? string("连接后20s") : to_string(g_option.loopTime / 1000) + "s") << " 后退出) ====";
    }

    void onServerAccecpt()
    {
        server_socket = server->nextPendingConnection();
        if (m_bench)
            m_bench->onConnected();
        connect(&server_socket->sigTextMessageReceived, this, &JZWebSocketTest::onServerRecv);
    }

    void onServerRecv(const string& text)
    {
        server_socket->sendTextMessage(text);   // 原样回传
        if (g_option.enableServer && !g_option.enableClient)   // 纯服务器模式: 在接收时计时
            m_tps.hit();
    }

    void onClientRecv(const string &text)
    {
        if (text.substr(0, WS_PREFIX.size()) != WS_PREFIX)
            netReportError("WebSocket", "非法消息格式 " + text);

        uint64_t recv_seq = stoull(text.substr(WS_PREFIX.size()));
        if (recv_seq != expect_recv_seq)
            netReportError("WebSocket", "单线程序号错位 期望=" + to_string(expect_recv_seq.load()) + " 实际=" + to_string(recv_seq));

        expect_recv_seq++;
        uint64_t current_seq = send_seq.fetch_add(1);
        client->sendTextMessage(WS_PREFIX + to_string(current_seq));
        if (g_option.enableClient)
            m_tps.hit();
    }

    void onClientConnect()
    {
        uint64_t current_seq = send_seq.fetch_add(1);
        client->sendTextMessage(WS_PREFIX + to_string(current_seq));
    }
};

// ===================== 客户端线程（独立运行） =====================
class WebSocketClientThread : public JZThread
{
public:
    JZWebSocket* client = nullptr;
    string serverUrl;
    int threadId = 0;
    NetTpsCounter *tps = nullptr;

    WebSocketClientThread(const string& url, int id) : serverUrl(url), threadId(id) 
    {
        moveToThread(this);
        connect(&sigFinished, this, &WebSocketClientThread::onFinished);
    }

    void onFinished()
    {
        client->disconnectFromHost();
        moveToThread(m_parent->thread());
    }

    void run() override {
        client = new JZWebSocket(this);
        connect(&client->sigTextMessageReceived, this, [this](const string& text) {
            sendMessage(text);
            tps->hit();
        });
        client->connectToHost(JZNetworkRequest(serverUrl));
        exec(); // 线程事件循环
    }

    void sendMessage(const string& text) {
        if (client) client->sendTextMessage(text);
    }
};

// ===================== 主线程：Server + 管理 N 个客户端线程 =====================
class JZWebSocketTestMultiThread : public JZObject
{
public:
    static const int CLIENT_THREAD_COUNT = 16;
    std::vector<WebSocketClientThread*> clientThreads;
    std::atomic<uint64_t> send_seq{0};
    std::map<JZWebSocket*, uint64_t> expect_recv_map;
    JZWebSocketServer* server = nullptr;
    int port = 18000;
    NetTpsCounter m_tps{"WebSocket 多线程"};
    NetServerBench *m_bench = nullptr;

    JZWebSocketTestMultiThread(JZEventLoop *loop, int p = 18001)
    {
        port = p;
        if (g_option.enableServer)
        {
            server = new JZWebSocketServer(this);
            server->listen(port);
            connect(&server->sigAccept, this, &JZWebSocketTestMultiThread::onServerAccept);
        }

        string url = "ws://" + g_option.host + ":" + to_string(port);
        if (g_option.enableClient)
        {
            for (int i = 0; i < CLIENT_THREAD_COUNT; i++) {
                auto thread = new WebSocketClientThread(url, i);
                thread->setParent(this);
                thread->tps = &m_tps;
                clientThreads.push_back(thread);
            }
            // 所有线程的 parent-child 关系建立完毕后再统一 start，
            // 避免前一个线程 run() 中 new JZWebSocket(this) 修改 m_childs
            // 与后一个线程的 setParent 遍历同一 m_childs 产生数据竞争
            for (auto t : clientThreads) {
                t->start();
            }
        }

        m_tps.begin();
        if (g_option.enableServer && !g_option.enableClient)   // 纯服务器模式: 等30s连接, 连接后跑20s
            m_bench = serverInstallExit(this, loop, 30000, 20000);
        else
            netInstallExit(this, loop, g_option.loopTime);
        jzDebug() << "==== WebSocket 多线程测试启动 (" << CLIENT_THREAD_COUNT << " 客户端, " << (g_option.enableServer && !g_option.enableClient ? string("连接后20s") : to_string(g_option.loopTime / 1000) + "s") << " 后退出) ====";
    }

    void stop()
    {
        for (auto t : clientThreads) {
            t->quit();
            t->wait();
            delete t;
        }
        clientThreads.clear();
    }

    ~JZWebSocketTestMultiThread() {
        stop();
    }

    void onServerAccept()
    {
        JZWebSocket* socket = server->nextPendingConnection();
        if (!socket)
            return;
        if (m_bench)
            m_bench->onConnected();

        connect(&socket->sigDisconnected, this, &JZWebSocketTestMultiThread::onClientDisConnect);
        connect(&socket->sigTextMessageReceived, this, &JZWebSocketTestMultiThread::onClientRecv);

        uint64_t curr = send_seq.fetch_add(1);
        expect_recv_map[socket] = curr;
        socket->sendTextMessage(WS_PREFIX + to_string(curr));
    }

    void onClientDisConnect()
    {
    }

    void onClientRecv(const string& text)
    {
        auto socket = (JZWebSocket*)sender();
        if (text.compare(0, WS_PREFIX.size(), WS_PREFIX) != 0)
            netReportError("WebSocket", "多线程非法格式 " + text);

        int expect = expect_recv_map[socket];
        uint64_t recv_seq = stoull(text.substr(WS_PREFIX.size()));
        if (recv_seq != expect)
            netReportError("WebSocket", "多线程序号错位 期望=" + to_string(expect) + " 实际=" + to_string(recv_seq));

        uint64_t curr = send_seq.fetch_add(1);
        expect_recv_map[socket] = curr;
        socket->sendTextMessage(WS_PREFIX + to_string(curr));
        if (g_option.enableServer && !g_option.enableClient)   // 纯服务器模式: 在接收时计时
            m_tps.hit();
    }
};

int test_websocket_single()
{
    g_netTestActive = true;
    JZEventLoop loop;
    JZWebSocketTest test(&loop);
    loop.exec();
    double tps = test.m_tps.average();
    netCheckTps("WebSocket 单线程", tps);
    g_netTestActive = false;
    JZThread::msleep(g_option.delayTime);
    return (int)tps;
}

int test_websocket_multi()
{
    g_netTestActive = true;
    JZEventLoop loop;
    JZWebSocketTestMultiThread test(&loop, 18001);
    loop.exec();
    g_netTestActive = false;

    test.stop();
    double tps = test.m_tps.average();
    netCheckTps("WebSocket 多线程", tps);
    JZThread::msleep(g_option.delayTime);
    return (int)tps;
}

int test_websocket()
{
    jzDebug() << "########## WebSocket 测试 (单线程 + 多线程, 各 15s) ##########";
    test_websocket_single();
    jzDebug() << "【OK】WebSocket 单线程通过";
    test_websocket_multi();
    jzDebug() << "【OK】WebSocket 多线程通过";
    return 0;
}
