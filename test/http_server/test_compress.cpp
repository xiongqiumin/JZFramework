#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

#include "JZDebug.h"
#include "JZEventLoop.h"
#include "JZTimer.h"
#include "JZByteArray.h"
#include "JZStd.h"
#include "JZHttpServer.h"
#include "JZHttpRequest.h"
#include "JZHttpRespone.h"
#include "JZHttpHandler.h"
#include "JZNetworkAccessManager.h"
#include "JZNetworkReply.h"
#include "JZNetworkRequest.h"
#include "JZApplication.h"
#include "JZCompress.h"
#include "JZStd.h"

using namespace std;

#define LOG(...) do { printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while(0)

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

// 把原始 body 压缩并构建响应（模拟服务端主动压缩 + 正确设置 Content-Encoding）
static JZByteArray buildCompressedResponse(const JZByteArray &payload,
                                           JZCompressAlgorithm algo,
                                           const string &encodingHeader)
{
    auto r = jzCompressBlock(payload, algo);
    if (!r.isOk())
        reportError("COMPRESS", "jzCompressBlock 失败");
    JZHttpRespone resp;
    resp.setStatusCode(200);
    resp.setContentType("text/plain");
    resp.setHeader("Content-Encoding", encodingHeader);
    resp.setBody(r.value());
    return resp.toByteArray();
}

static JZByteArray buildPlainResponse(const JZByteArray &payload)
{
    JZHttpRespone resp;
    resp.setStatusCode(200);
    resp.setContentType("text/plain");
    resp.setBody(payload);
    return resp.toByteArray();
}

// gzip：服务端总是压缩（不依赖请求头）
class GzipHandler : public JZHttpHandler
{
public:
    JZByteArray payload;
    void handle(JZHttpServerClient *client, const JZHttpRequest &req) override
    {
        (void)req;
        client->response(buildCompressedResponse(payload, JZCompressAlgorithm::GZip, "gzip"));
    }
};

// deflate：HTTP "deflate" 实为 zlib 封装的 raw deflate
class DeflateHandler : public JZHttpHandler
{
public:
    JZByteArray payload;
    void handle(JZHttpServerClient *client, const JZHttpRequest &req) override
    {
        (void)req;
        client->response(buildCompressedResponse(payload, JZCompressAlgorithm::Zlib, "deflate"));
    }
};

// 未压缩（对照）
class PlainHandler : public JZHttpHandler
{
public:
    JZByteArray payload;
    void handle(JZHttpServerClient *client, const JZHttpRequest &req) override
    {
        (void)req;
        client->response(buildPlainResponse(payload));
    }
};

// /auto：根据请求头 Accept-Encoding 决定压缩（服务端内容协商）
class AutoHandler : public JZHttpHandler
{
public:
    JZByteArray payload;
    void handle(JZHttpServerClient *client, const JZHttpRequest &req) override
    {
        string accept = str_toLower(req.header("Accept-Encoding"));
        if (accept.find("gzip") != string::npos)
            client->response(buildCompressedResponse(payload, JZCompressAlgorithm::GZip, "gzip"));
        else
            client->response(buildPlainResponse(payload));
    }
};

// ===================== 服务端压缩响应测试 =====================
class HttpCompressTest : public JZObject
{
public:
    HttpCompressTest(JZEventLoop *loop)
        : m_loop(loop)
    {
        int port = 18255;
        m_url = "http://127.0.0.1:" + to_string(port);

        // 原始载荷：放大到数十 KB，确保压缩真正生效（不只是帧头）
        string base = "JZHttpServer 压缩测试 / server compression payload 0123456789 "
                      "ABCDEFGHIJKLMNOPQRSTUVWXYZ 中文压缩数据 変換テスト ";
        string p;
        p.reserve(base.size() * 300);
        for (int i = 0; i < 300; i++)
            p += base;
        m_payload = JZByteArray(p.data(), (int)p.size());

        m_server = new JZHttpServer(this);
        m_server->listen(port);

        m_gzip = new GzipHandler();   m_gzip->payload = m_payload;
        m_deflate = new DeflateHandler(); m_deflate->payload = m_payload;
        m_plain = new PlainHandler(); m_plain->payload = m_payload;
        m_auto = new AutoHandler();   m_auto->payload = m_payload;

        m_server->addRoute("/gzip", m_gzip);
        m_server->addRoute("/deflate", m_deflate);
        m_server->addRoute("/plain", m_plain);
        m_server->addRoute("/auto", m_auto);

        m_net = new JZNetworkAccessManager(this);

        m_exitTimer = new JZTimer(this);
        m_exitTimer->setSingleShot(true);
        connect(&m_exitTimer->sigTimeout, this, [this]() {
            reportError("TIMEOUT", "压缩测试超时");
        });
        m_exitTimer->start(30 * 1000);

        LOG("[test] step0: GET /gzip (Content-Encoding: gzip)");
        m_step = 0;
        fetch("/gzip", "", "");
    }

    void fetch(const string &path, const string &acceptEncoding, const string &desc)
    {
        m_expect = m_payload;
        m_desc = desc;

        JZNetworkRequest req(m_url + path);
        if (!acceptEncoding.empty())
            req.setRawHeader("Accept-Encoding", acceptEncoding);
        JZNetworkReply *reply = m_net->get(req);
        connect(&reply->sigFinished, this, [this, reply]() { onFinished(reply); });
    }

    void onFinished(JZNetworkReply *reply)
    {
        if (reply->error() != JZNetworkReply::NoError &&
            reply->error() != JZNetworkReply::HttpError)
        {
            reportError("HTTP_COMPRESS", "net error: " + reply->errorString());
            return;
        }
        int code = reply->httpStatusCode();
        JZByteArray body = reply->readAll();
        if (code != 200)
            reportError("HTTP_COMPRESS", m_desc + " 状态码错误 期望=200 实际=" + to_string(code));
        if (!byteEqual(body, m_expect))
            reportError("HTTP_COMPRESS", m_desc + " 解压后 body 不匹配 期望长度=" +
                        to_string(m_expect.size()) + " 实际=" + to_string(body.size()));
        LOG("[test] %s OK (解压后 %d 字节)", m_desc.c_str(), (int)body.size());
        reply->deleteLater();

        if (m_step == 0)
        {
            m_step = 1;
            LOG("[test] step1: GET /deflate (Content-Encoding: deflate)");
            fetch("/deflate", "", "deflate");
        }
        else if (m_step == 1)
        {
            m_step = 2;
            LOG("[test] step2: GET /plain (对照, 不压缩)");
            fetch("/plain", "", "plain");
        }
        else if (m_step == 2)
        {
            m_step = 3;
            LOG("[test] step3: GET /auto + Accept-Encoding: gzip (协商压缩)");
            fetch("/auto", "gzip", "auto+gzip");
        }
        else if (m_step == 3)
        {
            m_step = 4;
            LOG("[test] step4: GET /auto 不带 Accept-Encoding (协商不压缩)");
            fetch("/auto", "", "auto+plain");
        }
        else
        {
            LOG("[test] ALL OK (gzip / deflate / plain / auto+gzip / auto+plain)");
            g_active = false;
            m_loop->quit();
            ::exit(0);   // 绕过可能的框架析构挂起, 直接退出
        }
    }

    JZEventLoop *m_loop;
    string m_url;
    JZHttpServer *m_server;
    JZNetworkAccessManager *m_net;
    JZTimer *m_exitTimer;
    JZByteArray m_payload;
    JZByteArray m_expect;
    string m_desc;
    int m_step = 0;

    GzipHandler *m_gzip;
    DeflateHandler *m_deflate;
    PlainHandler *m_plain;
    AutoHandler *m_auto;
};

int main()
{
    JZApplication a;
    LOG("########## HTTP 服务端压缩响应测试 (gzip / deflate / plain / auto) ##########");
    JZEventLoop loop;
    HttpCompressTest test(&loop);
    loop.exec();
    return 0;
}
