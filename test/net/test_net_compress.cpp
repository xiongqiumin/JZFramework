#include <cstdlib>
#include <string>
#include <vector>

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

#include "JZCompress.h"

using namespace std;

// 原始载荷：放大到数十 KB，确保压缩真正生效（不只是帧头）
static string makePayload()
{
    string base = "JZNet 压缩测试 / compression test payload 0123456789 "
                  "ABCDEFGHIJKLMNOPQRSTUVWXYZ 中文压缩数据 変換テスト ";
    string out;
    out.reserve(base.size() * 300);
    for (int i = 0; i < 300; i++)
        out += base;
    return out;
}

// ---- gzip 响应（HTTP Content-Encoding: gzip）----
class GzipHandler : public JZHttpHandler
{
public:
    string payload;
    void handle(JZHttpServerClient *client, const JZHttpRequest &req) override
    {
        (void)req;
        JZByteArray raw(payload.data(), (int)payload.size());
        auto r = jzCompressBlock(raw, JZCompressAlgorithm::GZip);
        if (!r.isOk())
        {
            client->response(JZByteArray());
            return;
        }
        JZHttpRespone resp;
        resp.setStatusCode(200);
        resp.setContentType("text/plain");
        resp.setHeader("Content-Encoding", "gzip");
        resp.setBody(r.value());
        client->response(resp.toByteArray());
    }
};

// ---- deflate 响应（HTTP "deflate" 实为 zlib 封装的 raw deflate）----
class DeflateHandler : public JZHttpHandler
{
public:
    string payload;
    void handle(JZHttpServerClient *client, const JZHttpRequest &req) override
    {
        (void)req;
        JZByteArray raw(payload.data(), (int)payload.size());
        auto r = jzCompressBlock(raw, JZCompressAlgorithm::Zlib);
        if (!r.isOk())
        {
            client->response(JZByteArray());
            return;
        }
        JZHttpRespone resp;
        resp.setStatusCode(200);
        resp.setContentType("text/plain");
        resp.setHeader("Content-Encoding", "deflate");
        resp.setBody(r.value());
        client->response(resp.toByteArray());
    }
};

// ---- 未压缩（对照）----
class PlainHandler : public JZHttpHandler
{
public:
    string payload;
    void handle(JZHttpServerClient *client, const JZHttpRequest &req) override
    {
        (void)req;
        JZHttpRespone resp;
        resp.setStatusCode(200);
        resp.setContentType("text/plain");
        resp.setBody(JZByteArray(payload.data(), (int)payload.size()));
        client->response(resp.toByteArray());
    }
};

// 顺序发起 gzip / deflate / plain 三个请求，逐一校验解压后的 body 与原始载荷一致
class CompressTest : public JZObject
{
public:
    CompressTest(JZEventLoop *loop, int port)
        : m_loop(loop), m_port(port)
    {
        m_payload = makePayload();

        auto *gzip = new GzipHandler();  gzip->payload = m_payload;
        auto *defl = new DeflateHandler(); defl->payload = m_payload;
        auto *plain = new PlainHandler(); plain->payload = m_payload;

        m_server = new JZHttpServer(this);
        m_server->listen(port);
        m_server->addRoute("/gzip", gzip);
        m_server->addRoute("/deflate", defl);
        m_server->addRoute("/plain", plain);

        m_net = new JZNetworkAccessManager(this);

        string base = "http://127.0.0.1:" + to_string(port);
        m_urls = { base + "/gzip", base + "/deflate", base + "/plain" };
        m_kinds = { "gzip", "deflate", "plain" };

        // 看门狗：10s 内未全部完成则判失败退出
        JZTimer::singleShot(10000, this, [this]() {
            jzDebug() << "【FAIL】压缩测试超时，未在规定时间内完成";
            exit(1);
        });

        sendNext();
    }

    void sendNext()
    {
        if (m_step >= (int)m_urls.size())
        {
            jzDebug() << "【OK】压缩测试全部通过 (gzip / deflate / plain)";
            m_loop->quit();
            return;
        }
        JZNetworkRequest req(m_urls[m_step]);
        JZNetworkReply *reply = m_net->get(req);
        connect(&reply->sigFinished, this, &CompressTest::onReply);
    }

    void onReply()
    {
        JZNetworkReply *reply = (JZNetworkReply *)sender();
        string kind = m_kinds[m_step];
        bool ok = (reply->error() == JZNetworkReply::NoError);
        string got = ok ? str_fromUtf8(reply->readAll()) : string();
        if (!ok || got != m_payload)
        {
            jzDebug() << "【FAIL】" << kind << " 解压校验失败 error=" << (int)reply->error()
                      << " 期望长度=" << m_payload.size() << " 收到长度=" << got.size();
            reply->deleteLater();
            exit(1);
        }
        jzDebug() << "【OK】" << kind << " 解压正确 (原始 " << m_payload.size()
                  << " 字节 -> 解压后 " << got.size() << " 字节)";
        reply->deleteLater();
        m_step++;
        sendNext();
    }

    JZEventLoop *m_loop;
    int m_port;
    string m_payload;
    JZHttpServer *m_server;
    JZNetworkAccessManager *m_net;
    vector<string> m_urls;
    vector<string> m_kinds;
    int m_step = 0;
};

int main()
{
    jzDebug() << "########## JZNet 压缩响应测试 (gzip / deflate / plain) ##########";
    JZEventLoop loop;
    CompressTest test(&loop, 18299);
    loop.exec();
    return 0;
}
