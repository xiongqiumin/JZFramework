// 来源地址: JZHttpRequest::remoteAddr()
//   恒等于 **TCP 对端 IP**。
//   请求头 X-Forwarded-For 是客户端可完全控制的数据, "要不要相信它"属于部署策略
//   (前面有没有反代、端口是否只对反代可达), 由应用层自己决定 —— 框架若默认采用,
//   等于让任何客户端都能伪装来源(按 IP 的失败封禁会因此失效)。
//   需要 XFF 的场合在 handler 里读 req.header("X-Forwarded-For")。
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
            LOG("[test] 直连 OK (%s), step1: 带 X-Forwarded-For 也必须是 TCP 对端", body.c_str());
            m_step = 1;
            fetch("10.1.2.3", "127.0.0.1");
            return;
        }
        if (m_step == 1)
        {
            LOG("[test] 单段不采纳 OK (%s), step2: 多段 X-Forwarded-For 同样不采纳", body.c_str());
            m_step = 2;
            fetch("10.1.2.3, 10.0.0.1, 172.16.0.9", "127.0.0.1");
            return;
        }

        LOG("[test] 多段不采纳 OK (%s), ALL DONE", body.c_str());
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
    LOG("########## HTTP 来源地址测试 (remoteAddr 恒为 TCP 对端) ##########");
    g_active = true;
    JZEventLoop loop;
    PeerTest test(&loop);
    loop.exec();
    return 0;
}
