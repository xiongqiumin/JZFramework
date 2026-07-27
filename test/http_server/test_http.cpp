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
#include "JZHttpStaticFiles.h"
#include "JZNetworkAccessManager.h"
#include "JZNetworkReply.h"
#include "JZNetworkRequest.h"
#include "JZApplication.h"
#include "JZDir.h"

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

static bool byteEqual(const JZByteArray &a, const JZByteArray &b)
{
    if (a.size() != b.size())
        return false;
    return memcmp(a.data(), b.data(), (size_t)a.size()) == 0;
}

// 生成确定性二进制内容(覆盖全字节, 含 0x00), 用于校验分块发送字节级一致
static JZByteArray makeContent(int size)
{
    JZByteArray b;
    b.resize(size);
    char *p = b.data();
    for (int i = 0; i < size; i++)
        p[i] = (char)((i * 31 + 7) & 0xFF);
    return b;
}

static void ensureDir(const string &path)
{
    JZDir dir;
    dir.mkpath(path);
}

static void writeFile(const string &path, const JZByteArray &data)
{
    FILE *fp = fopen(path.c_str(), "wb");
    if (!fp)
    {
        reportError("PREPARE", "无法创建测试文件 " + path);
        return;
    }
    fwrite(data.data(), 1, (size_t)data.size(), fp);
    fclose(fp);
}

// 回声 Handler: 原样回传请求 body
class HttpEchoHandler : public JZHttpHandler
{
public:
    void handle(JZHttpServerClient *client, const JZHttpRequest &req) override
    {
        JZByteArray body = req.body();
        string header = "HTTP/1.1 200 OK\r\n"
                        "Content-Type: text/plain\r\n"
                        "Content-Length: " + to_string(body.size()) + "\r\n"
                        "Connection: close\r\n\r\n";
        JZByteArray resp;
        resp.append(str_toUtf8(header));
        resp.append(body);
        client->write(resp.data(), (int)resp.size(), true);
    }
};

// SSE 推送 Handler: 与 test_sse.cpp 一致, 以 chunked 方式推送 5 个事件
class SseHandler : public JZHttpHandler
{
public:
    void handle(JZHttpServerClient *client, const JZHttpRequest &req) override
    {
        string header =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream\r\n"
            "Cache-Control: no-cache\r\n"
            "Transfer-Encoding: chunked\r\n"
            "Connection: keep-alive\r\n"
            "\r\n";
        client->write(header.c_str(), (int)header.size(), false);

        int *count = new int(0);
        JZTimer *timer = new JZTimer(client);
        timer->setSingleShot(false);
        connect(&timer->sigTimeout, client, [client, count, timer]() {
            (*count)++;
            string frame = "event: tick\r\ndata: " + to_string(*count) +
                           "\r\nid: " + to_string(*count) + "\r\n\r\n";
            if (*count <= 5)
            {
                char lenbuf[32];
                snprintf(lenbuf, sizeof(lenbuf), "%zx\r\n", frame.size());
                string chunk = string(lenbuf) + frame + "\r\n";
                client->write(chunk.c_str(), (int)chunk.size(), false);
            }
            else
            {
                client->write("0\r\n\r\n", 5, true);
                delete timer;
                delete count;
            }
        });
        timer->start(50);
    }
};

// ===================== 综合测试: 静态文件 + SSE + Echo 串起来 =====================
class HttpAllTest : public JZObject
{
public:
    HttpAllTest(JZEventLoop *loop)
        : m_loop(loop)
    {
        int port = 18222;
        m_url = "http://127.0.0.1:" + to_string(port);

        prepareFiles();

        m_server = new JZHttpServer(this);
        m_server->listen(port);

        JZHttpStaticFiles *static_handler = new JZHttpStaticFiles(m_root);
        static_handler->setChunkSize(8192);
        m_server->addRoute("/<path:path>", static_handler);
        m_server->addRoute("/sse", new SseHandler());
        m_server->addRoute("/echo", new HttpEchoHandler());

        m_net = new JZNetworkAccessManager(this);

        m_exitTimer = new JZTimer(this);
        m_exitTimer->setSingleShot(true);
        connect(&m_exitTimer->sigTimeout, this, [this]() { m_loop->quit(); });
        m_exitTimer->start(30 * 1000);

        LOG("[test] step0: GET /big.bin (静态分块流式)");
        m_step = 0;
        fetch("/big.bin", 200, m_expect);
    }

    void prepareFiles()
    {
        m_root = jzApp->applicationDirPath() + "/http_all_root";
        ensureDir(m_root);
        m_expect = makeContent(50000);
        writeFile(m_root + "/big.bin", m_expect);
    }

    void fetch(const string &path, int expectCode, const JZByteArray &expectBody,
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
            reportError("HTTP_ALL", "net error: " + reply->errorString());
            return;
        }

        int code = reply->httpStatusCode();
        JZByteArray body = reply->readAll();
        if (code != m_expectCode)
            reportError("HTTP_ALL", "状态码错误 期望=" + to_string(m_expectCode) + " 实际=" + to_string(code));
        if (!byteEqual(body, m_expectBody))
            reportError("HTTP_ALL", "body 不匹配 期望长度=" + to_string(m_expectBody.size()) + " 实际=" + to_string(body.size()));
        reply->deleteLater();

        if (m_step == 0)
        {
            // 静态文件通过, 进入 SSE 流式测试
            LOG("[test] big.bin OK, step1: GET /sse (SSE 流式)");
            m_step = 1;
            JZNetworkRequest req(m_url + "/sse");
            JZNetworkReply *reply = m_net->get(req);
            connect(&reply->sigFinished, this, [this, reply]() { onSseFinished(reply); });
        }
        else if (m_step == 2)
        {
            // Echo 通过, 全部完成
            LOG("[test] echo OK, ALL DONE");
            g_active = false;
            m_loop->quit();
            ::exit(0);   // 绕过可能的框架析构挂起, 直接退出
        }
    }

    void onSseFinished(JZNetworkReply *reply)
    {
        if (reply->error() != JZNetworkReply::NoError &&
            reply->error() != JZNetworkReply::HttpError)
        {
            reportError("HTTP_ALL", "sse net error: " + reply->errorString());
            return;
        }

        int code = reply->httpStatusCode();
        string body = str_fromUtf8(reply->readAll());
        if (code != 200)
            reportError("HTTP_ALL", "sse 状态码错误 期望=200 实际=" + to_string(code));
        for (int i = 1; i <= 5; i++)
        {
            if (body.find("event: tick\r\n") == string::npos)
                reportError("HTTP_ALL", "sse 缺少 event: tick");
            if (body.find("data: " + to_string(i) + "\r\n") == string::npos)
                reportError("HTTP_ALL", "sse 缺少 data: " + to_string(i));
            if (body.find("id: " + to_string(i) + "\r\n") == string::npos)
                reportError("HTTP_ALL", "sse 缺少 id: " + to_string(i));
        }
        LOG("[test] sse OK (%d bytes), step2: POST /echo", (int)body.size());
        reply->deleteLater();

        // SSE 通过, 进入 Echo 测试
        m_step = 2;
        fetch("/echo", 200, str_toUtf8("ping"), "POST", str_toUtf8("ping"));
    }

    JZEventLoop *m_loop;
    string m_url, m_root;
    JZHttpServer *m_server;
    JZNetworkAccessManager *m_net;
    JZTimer *m_exitTimer;
    JZByteArray m_expect;
    int m_expectCode = 200;
    JZByteArray m_expectBody;
    int m_step = 0;
};

int test_http()
{
    g_active = true;
    JZEventLoop loop;
    HttpAllTest test(&loop);
    loop.exec();
    return 0;
}

int main()
{
    JZApplication a;
    LOG("########## HTTP 综合测试 (静态分块 + SSE 流式 + Echo) ##########");
    return test_http();
}
