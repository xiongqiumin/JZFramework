// JZHttpServer 路由集成测试：用真实服务器 + JZNetworkAccessManager 验证
// 精确/动态/中途参数路由、优先级、query、POST body、404。
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

#include "JZDebug.h"
#include "JZEventLoop.h"
#include "JZTimer.h"
#include "JZByteArray.h"
#include "JZHttpServer.h"
#include "JZHttpHandler.h"
#include "JZNetworkAccessManager.h"
#include "JZNetworkReply.h"
#include "JZNetworkRequest.h"
#include "JZApplication.h"

using namespace std;

#define LOG(...) do { printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

static atomic<bool> g_active{true};

static void reportError(const string &where, const string &detail)
{
    if (!g_active.load())
        return;
    jzDebug() << "[FATAL][" << where << "] " << detail;
    exit(1);
}

static void sendText(JZHttpServerClient *c, const string &body, int code = 200)
{
    JZHttpRespone resp;
    resp.setStatusCode(code);
    resp.setHeader("Content-Type", "text/plain; charset=utf-8");
    resp.setBody(body);
    c->response(resp.toByteArray());   // toByteArray 自动补充 Content-Length
}

class RootHandler : public JZHttpHandler
{
public:
    void handle(JZHttpServerClient *client, const JZHttpRequest &req) override
    {
        sendText(client, "root");
    }
};

class UserHandler : public JZHttpHandler
{
public:
    void handle(JZHttpServerClient *client, const JZHttpRequest &req) override
    {
        sendText(client, "user:" + req.param("id"));
    }
};

class PostsHandler : public JZHttpHandler
{
public:
    void handle(JZHttpServerClient *client, const JZHttpRequest &req) override
    {
        sendText(client, "post:" + req.param("id"));
    }
};

class SearchHandler : public JZHttpHandler
{
public:
    void handle(JZHttpServerClient *client, const JZHttpRequest &req) override
    {
        const auto &q = req.queryParams();
        auto it = q.find("q");
        string val = (it != q.end()) ? it->second : string();
        sendText(client, val);
    }
};

class EchoHandler : public JZHttpHandler
{
public:
    void handle(JZHttpServerClient *client, const JZHttpRequest &req) override
    {
        JZByteArray b = req.body();
        sendText(client, string(b.data(), b.size()));
    }
};

class RoutingTest : public JZObject
{
public:
    RoutingTest(JZEventLoop *loop)
        : m_loop(loop)
    {
        int port = 18333;
        m_url = "http://127.0.0.1:" + to_string(port);

        m_server = new JZHttpServer(this);
        m_server->addRoute("/", new RootHandler());
        m_server->addRoute("/user/<id>", new UserHandler());
        m_server->addRoute("/user/<id>/posts", new PostsHandler());
        m_server->addRoute("/search", new SearchHandler());
        m_server->addRoute("/echo", new EchoHandler());
        if (!m_server->listen(port))
            reportError("ROUTING", "listen 失败");

        m_net = new JZNetworkAccessManager(this);

        m_exitTimer = new JZTimer(this);
        m_exitTimer->setSingleShot(true);
        connect(&m_exitTimer->sigTimeout, this, [this]() { m_loop->quit(); });
        m_exitTimer->start(30 * 1000);

        LOG("[test] step0: GET /");
        m_step = 0;
        fetch("/", 200, "root");
    }

    void fetch(const string &path, int expectCode = 200, const string &expectBody = "",
               const string &method = "GET", const JZByteArray &postBody = JZByteArray())
    {
        m_expectCode = expectCode;
        m_expectBody = expectBody;

        JZNetworkRequest req(m_url + path);
        JZNetworkReply *reply;
        if (method == "POST")
            reply = m_net->post(req, postBody);
        else
            reply = m_net->get(req);
        connect(&reply->sigFinished, this, [this, reply]() { onFinished(reply); });
    }

    void onFinished(JZNetworkReply *reply)
    {
        if (reply->error() != JZNetworkReply::NoError &&
            reply->error() != JZNetworkReply::HttpError)
        {
            reportError("ROUTING", "net error: " + reply->errorString());
            return;
        }
        int code = reply->httpStatusCode();
        string body = str_fromUtf8(reply->readAll());
        if (code != m_expectCode)
            reportError("ROUTING", "状态码错误 期望=" + to_string(m_expectCode) + " 实际=" + to_string(code));
        if (body != m_expectBody)
            reportError("ROUTING", "body 不匹配 期望=[" + m_expectBody + "] 实际=[" + body + "]");
        reply->deleteLater();

        if (m_step == 0)
        {
            LOG("[test] / -> root OK, step1: GET /user/42");
            m_step = 1;
            fetch("/user/42", 200, "user:42");
        }
        else if (m_step == 1)
        {
            LOG("[test] /user/42 OK, step2: GET /user/42/posts");
            m_step = 2;
            fetch("/user/42/posts", 200, "post:42");
        }
        else if (m_step == 2)
        {
            LOG("[test] /user/42/posts OK, step3: GET /search?q=hello");
            m_step = 3;
            fetch("/search?q=hello", 200, "hello");
        }
        else if (m_step == 3)
        {
            LOG("[test] /search?q=hello OK, step4: POST /echo");
            m_step = 4;
            fetch("/echo", 200, "payload123", "POST", str_toUtf8("payload123"));
        }
        else if (m_step == 4)
        {
            LOG("[test] POST /echo OK, step5: GET /no-such-route (404)");
            m_step = 5;
            fetch("/no-such-route", 404, "Not Found");
        }
        else
        {
            LOG("[test] ALL OK");
            g_active = false;
            m_loop->quit();
            ::exit(0);
        }
    }

    JZEventLoop *m_loop;
    string m_url;
    JZHttpServer *m_server;
    JZNetworkAccessManager *m_net;
    JZTimer *m_exitTimer;
    int m_expectCode = 200;
    string m_expectBody;
    int m_step = 0;
};

int test_routing()
{
    LOG("########## HTTP 路由集成测试 ##########");
    JZEventLoop loop;
    RoutingTest test(&loop);
    loop.exec();
    return 0;
}
