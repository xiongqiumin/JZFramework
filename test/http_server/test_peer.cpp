// 来源地址: JZHttpRequest::remoteAddr()
//   直连        -> TCP 对端 IP
//   反向代理    -> X-Forwarded-For 第一段(nginx: $proxy_add_x_forwarded_for)
// 以前 remoteAddr() 恒为空串(setRemoteAddr 没有任何调用点)。

#include <atomic>
#include <cstdio>
#include <string>

#include "JZApplication.h"
#include "JZEventLoop.h"
#include "JZTimer.h"
#include "JZByteArray.h"
#include "JZDebug.h"
#include "JZHttpServer.h"
#include "JZHttpHandler.h"
#include "JZNetworkAccessManager.h"
#include "JZNetworkReply.h"
#include "JZNetworkRequest.h"
#include "JZStd.h"

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

// 原样回显请求的来源地址
class PeerHandler : public JZHttpHandler
{
public:
    void handle(JZHttpServerClient *client, const JZHttpRequest &req) override
    {
        JZHttpRespone resp;
        resp.setText(req.remoteAddr());
        client->response(resp, req);
    }
};

class PeerTest : public JZObject
{
public:
    PeerTest(JZEventLoop *loop)
        : m_loop(loop)
    {
        int port = 18232;
        m_url = "http://127.0.0.1:" + to_string(port);

        m_server = new JZHttpServer(this);
        if (!m_server->listen(port))
            fail("SETUP", "服务端 listen 失败");
        m_server->addRoute("/peer", new PeerHandler());

        m_net = new JZNetworkAccessManager(this);

        m_exitTimer = new JZTimer(this);
        m_exitTimer->setSingleShot(true);
        connect(&m_exitTimer->sigTimeout, this, [this]() {
            fail("TIMEOUT", "测试超时");
            m_loop->quit();
        });
        m_exitTimer->start(20 * 1000);

        LOG("[test] step0: 直连 -> 期望 TCP 对端 127.0.0.1");
        m_step = 0;
        fetch("", "127.0.0.1");
    }

    void fetch(const string &xff, const string &expect)
    {
        m_expect = expect;
        JZNetworkRequest req(m_url + "/peer");
        if (!xff.empty())
            req.setRawHeader("X-Forwarded-For", xff);
        JZNetworkReply *reply = m_net->get(req);
        connect(&reply->sigFinished, this, [this, reply]() { onFinished(reply); });
    }

    void onFinished(JZNetworkReply *reply)
    {
        if (reply->error() != JZNetworkReply::NoError &&
            reply->error() != JZNetworkReply::HttpError)
            fail("HTTP", "net error: " + reply->errorString());

        int code = reply->httpStatusCode();
        string body = str_fromUtf8(reply->readAll());
        reply->deleteLater();
        if (code != 200)
            fail("HTTP", "状态码期望=200 实际=" + to_string(code));
        if (body != m_expect)
            fail("PEER", "来源地址期望='" + m_expect + "' 实际='" + body + "'");

        if (m_step == 0)
        {
            LOG("[test] 直连 OK (%s), step1: X-Forwarded-For 单段", body.c_str());
            m_step = 1;
            fetch("10.1.2.3", "10.1.2.3");
            return;
        }
        if (m_step == 1)
        {
            LOG("[test] 单段 OK (%s), step2: X-Forwarded-For 多段取第一段", body.c_str());
            m_step = 2;
            fetch("10.1.2.3, 10.0.0.1, 172.16.0.9", "10.1.2.3");
            return;
        }

        LOG("[test] 多段 OK (%s), ALL DONE", body.c_str());
        g_active = false;
        m_loop->quit();
        ::exit(0);
    }

    JZEventLoop *m_loop;
    string m_url;
    JZHttpServer *m_server;
    JZNetworkAccessManager *m_net;
    JZTimer *m_exitTimer;
    string m_expect;
    int m_step = 0;
};

int test_peer()
{
    LOG("########## HTTP 来源地址测试 (TCP 对端 / X-Forwarded-For) ##########");
    g_active = true;
    JZEventLoop loop;
    PeerTest test(&loop);
    loop.exec();
    return 0;
}
