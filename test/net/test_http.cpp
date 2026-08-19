#include <atomic>
#include <chrono>
#include <string>
#include <vector>

#include "JZDebug.h"
#include "JZTimer.h"
#include "JZEventLoop.h"
#include "JZThread.h"
#include "JZByteArray.h"

#include "JZHttpServer.h"
#include "JZHttpRequest.h"
#include "JZHttpRespone.h"
#include "JZHttpHandler.h"
#include "JZNetworkAccessManager.h"
#include "JZNetworkReply.h"
#include "JZNetworkRequest.h"

#include "net_test_common.h"

using namespace std;

// ===================== 回声 Handler: 原样回传请求 body =====================
class HttpEchoHandler : public JZHttpHandler
{
public:
    NetTpsCounter *serverTps = nullptr;   // 纯服务器模式时由测试注入, 用于接收计时
    NetServerBench *bench = nullptr;      // 纯服务器模式时由测试注入, 用于首次连接计时

    void handle(JZHttpServerClient *client, const JZHttpRequest &req) override
    {
        JZHttpRespone resp;
        resp.setStatusCode(200);
        resp.setContentType("text/plain");
        resp.setBody(req.body());   // 原样回显 body
        if (serverTps)
            serverTps->hit();       // 服务器接收时计时(纯服务器模式由测试注入)
        if (bench || m_first)
        {
            bench->onConnected();   // 首次请求视为连接到达, 启动 run 计时
            m_first = false;
        }
        client->response(resp.toByteArray());
    }

    bool m_first = true;
};

// ===================== 单线程测试(主事件循环内跑一个客户端) =====================
class HttpTestSingle : public JZObject
{
public:
    HttpTestSingle(JZEventLoop *loop, int port)
        : m_port(port)
    {
        m_prefix = "http";
        m_url = "http://" + g_option.host + ":" + to_string(port);

        if (g_option.enableServer)
        {
            m_server = new JZHttpServer(this);
            m_server->listen(port);
            auto *h = new HttpEchoHandler();
            if (!g_option.enableClient)   // 纯服务器模式: 用服务器接收计时
            {
                h->serverTps = &m_tps;
                m_bench = serverInstallExit(this, loop, 30000, 20000);   // 等30s连接, 连接后跑20s
                h->bench = m_bench;
            }
            m_server->addRoute("/test", h);
        }

        if (g_option.enableClient)
        {
            m_net = new JZNetworkAccessManager(this);
        }

        m_tps.begin();
        if (g_option.enableServer && !g_option.enableClient)
        {
            // 纯服务器模式退出计时由 m_bench 控制
        }
        else
            netInstallExit(this, loop, g_option.loopTime);
        jzDebug() << "==== HTTP 单线程测试启动 (" << (g_option.enableServer && !g_option.enableClient ? string("连接后20s") : to_string(g_option.loopTime / 1000) + "s") << " 后退出) ====";
        if (g_option.enableClient)
            sendNext();
    }

    void sendNext()
    {
        JZNetworkRequest req(m_url + "/test");
        JZByteArray body = str_toUtf8(netMakeMsg(m_prefix, m_sendSeq));
        JZNetworkReply *reply = m_net->post(req, body);
        connect(&reply->sigFinished, this, &HttpTestSingle::onReply);
        m_sendSeq++;
    }

    void onReply()
    {
        JZNetworkReply *reply = (JZNetworkReply *)sender();
        if (reply->error() != JZNetworkReply::NoError)
            netReportError("HTTP", "请求失败 code=" + to_string((int)reply->error()));
        uint64_t seq;
        string body = str_fromUtf8(reply->readAll());
        if (!netParseMsg(body, m_prefix, seq) || seq != m_expect)
            netReportError("HTTP", "回显校验失败 期望=" + to_string(m_expect) + " 收到=" + body);
        m_expect++;
        if (g_option.enableClient)
            m_tps.hit();
        reply->deleteLater();

        //现在没有 keep alive, 不停发送会耗光资源. 限制一下
        JZTimer::singleShot(10, this, [this]() { sendNext(); });   // 每 100ms 发一个
    }

    int m_port;
    string m_prefix = "http";
    string m_url;
    JZHttpServer *m_server;
    JZNetworkAccessManager *m_net;
    NetTpsCounter m_tps{"HTTP 单线程"};
    NetServerBench *m_bench = nullptr;
    uint64_t m_sendSeq = 0;
    uint64_t m_expect = 0;
};

// ===================== 客户端线程: 各自独立事件循环, 并发请求 =====================
class HttpTestClientThread : public JZThread
{
public:
    HttpTestClientThread(const string &url, NetTpsCounter *tps, uint64_t startSeq)
        : m_url(url), m_tps(tps), m_startSeq(startSeq)
    {
        moveToThread(this);
        connect(&sigFinished, this, &HttpTestClientThread::onFinished);
    }

    void onFinished()
    {
        moveToThread(m_parent->thread());
    }

    void run() override
    {
        m_prefix = "http";
        m_net = new JZNetworkAccessManager(this);
        m_sendSeq = m_startSeq;
        m_expect = m_startSeq;
        sendNext();
        exec();   // 线程事件循环
        delete m_net;
        m_net = nullptr;
    }

    void sendNext()
    {
        JZNetworkRequest req(m_url + "/test");
        JZByteArray body = str_toUtf8(netMakeMsg(m_prefix, m_sendSeq));
        JZNetworkReply *reply = m_net->post(req, body);
        connect(&reply->sigFinished, this, &HttpTestClientThread::onReply);
        m_sendSeq++;
    }

    void onReply()
    {
        JZNetworkReply *reply = (JZNetworkReply *)sender();
        if (reply->error() != JZNetworkReply::NoError)
            netReportError("HTTP", "多线程请求失败 code=" + to_string((int)reply->error()));
        uint64_t seq;
        string body = str_fromUtf8(reply->readAll());
        if (!netParseMsg(body, m_prefix, seq) || seq != m_expect)
            netReportError("HTTP", "多线程回显校验失败 期望=" + to_string(m_expect) + " 收到=" + body);
        m_expect++;
        if (g_option.enableClient)
            m_tps->hit();
        reply->deleteLater();
        JZTimer::singleShot(10, this, [this]() {
            if(this->isRunning())
                sendNext(); 
        });   // 每 100ms 发一个
    }

    string m_url;
    NetTpsCounter *m_tps;
    uint64_t m_startSeq;
    JZNetworkAccessManager *m_net = nullptr;
    string m_prefix = "http";
    uint64_t m_sendSeq = 0;
    uint64_t m_expect = 0;
};

// ===================== 主线程: 独立 Server + 管理 N 个客户端线程 =====================
class HttpTestMultiThread : public JZObject
{
public:
    static const int CLIENT_THREAD_COUNT = 8;
    vector<HttpTestClientThread *> threads;
    JZHttpServer *m_server;
    NetTpsCounter m_tps{"HTTP 多线程"};

    HttpTestMultiThread(JZEventLoop *loop, int port)
    {
        HttpEchoHandler *http_handle = nullptr;
        string url = "http://" + g_option.host + ":" + to_string(port);
        if (g_option.enableServer)
        {
            m_server = new JZHttpServer(this);
            m_server->listen(port);
            http_handle = new HttpEchoHandler();
            m_server->addRoute("/test", http_handle);
        }

        if (g_option.enableClient)
        {
            for (int i = 0; i < CLIENT_THREAD_COUNT; i++)
            {
                auto t = new HttpTestClientThread(url, &m_tps, i * 1000000ULL);
                t->setParent(this);
                t->start();
                threads.push_back(t);
            }
        }

        m_tps.begin();
        if (g_option.enableServer && !g_option.enableClient)
        {
            // 纯服务器模式退出计时由 m_bench 控制
            http_handle->serverTps = &m_tps;
            m_bench = serverInstallExit(this, loop, 30000, 20000);   // 等30s连接, 连接后跑20s
            http_handle->bench = m_bench;
        }
        else
        {
            jzDebug() << "client loop" << g_option.loopTime;
            netInstallExit(this, loop, g_option.loopTime);
        }
        jzDebug() << "==== HTTP 多线程测试启动 (" << CLIENT_THREAD_COUNT << " 客户端, " << (g_option.enableServer && !g_option.enableClient ? string("连接后20s") : to_string(g_option.loopTime / 1000) + "s") << " 后退出) ====";
    }

    ~HttpTestMultiThread()
    {
    }

    void stop()
    {
        for (auto t : threads)
        {
            t->quit();
            t->wait();
            delete t;
        }
    }

    NetServerBench *m_bench = nullptr;
};

int test_http_single()
{
    g_netTestActive = true;
    JZEventLoop loop;
    HttpTestSingle test(&loop, 18104);
    loop.exec();
    double tps = test.m_tps.average();
    netCheckTps("HTTP 单线程", tps, 5);
    g_netTestActive = false;
    JZThread::msleep(g_option.delayTime);
    return (int)tps;
}

int test_http_multi()
{
    g_netTestActive = true;
    JZEventLoop loop;
    HttpTestMultiThread test(&loop, 18108);
    loop.exec();
    g_netTestActive = false;

    test.stop();

    double tps = test.m_tps.average();
    netCheckTps("HTTP 多线程", tps, 5);

    JZThread::msleep(g_option.delayTime);
    return (int)tps;
}

int test_http()
{
    jzDebug() << "########## HTTP 测试 (单线程 + 多线程, 各 15s) ##########";
    test_http_single();
    jzDebug() << "【OK】HTTP 单线程通过";
    test_http_multi();
    jzDebug() << "【OK】HTTP 多线程通过";
    return 0;
}
