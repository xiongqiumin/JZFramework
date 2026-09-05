#include <cstring>
#include <string>

#include "JZDebug.h"
#include "JZApplication.h"
#include "JZEventLoop.h"
#include "JZTimer.h"
#include "JZByteArray.h"

#include "JZNetworkAccessManager.h"
#include "JZNetworkReply.h"
#include "JZNetworkRequest.h"

using namespace std;

// 阻塞式 GET: 在内部事件循环上等待 sigFinished / 超时, 返回解压后的 body 字节
class CompressGet : public JZObject
{
public:
    explicit CompressGet(JZNetworkAccessManager *mgr) : m_mgr(mgr) {}

    bool run(const string &url, int timeoutMs, JZByteArray &body, string &err)
    {
        JZEventLoop loop;
        m_loop = &loop;
        m_done = false;
        m_ok = false;
        m_err.clear();
        m_body.clear();
        m_encoding.clear();

        JZNetworkRequest req(url);
        JZNetworkReply *reply = m_mgr->get(req);
        connect(&reply->sigFinished, this, &CompressGet::onReply);

        JZTimer timer(this);
        timer.setSingleShot(true);
        connect(&timer.sigTimeout, this, &CompressGet::onTimeout);
        timer.start(timeoutMs);

        loop.exec();

        body = m_body;
        err = m_err;
        return m_ok;
    }

protected:
    void onReply()
    {
        JZNetworkReply *reply = (JZNetworkReply *)sender();
        m_done = true;
        if (reply->error() != JZNetworkReply::NoError)
        {
            m_ok = false;
            m_err = reply->errorString();
        }
        else
        {
            m_ok = true;
            m_body = reply->readAll();   // 客户端已按 Content-Encoding 自动解压
            m_encoding = reply->header("content-encoding");
        }
        reply->deleteLater();
        if (m_loop)
            m_loop->quit();
    }

    void onTimeout()
    {
        if (!m_done)
        {
            m_ok = false;
            m_err = "timeout";
            if (m_loop)
                m_loop->quit();
        }
    }

    JZNetworkAccessManager *m_mgr;
    JZEventLoop *m_loop = nullptr;
    bool m_done = false;
    bool m_ok = false;
    JZByteArray m_body;
    string m_err;
    string m_encoding;   // 捕获响应 Content-Encoding，用于验证内容协商

public:
    const string& encoding() const { return m_encoding; }
};

static bool bytesEqual(const JZByteArray &a, const JZByteArray &b)
{
    if (a.size() != b.size())
        return false;
    if (a.size() == 0)
        return true;
    return memcmp(a.data(), b.data(), (size_t)a.size()) == 0;
}

int main()
{
    JZApplication a;   // 必须先创建，否则 JZObject 构造时 jzApp->currentThread() 空指针崩溃
    jzDebug() << "########## JZNet 压缩响应测试 (gzip / deflate / plain) ##########";

    // 连本地 net_test_server.py 的 HTTP 目标站（与 test_proxy 共用 8080）
    const int timeoutMs = 30000;
    const string base = "http://127.0.0.1:8080";
    JZNetworkAccessManager mgr;

    JZByteArray expected;
    string err;
    bool allOk = true;

    // 1) 未压缩基准载荷
    {
        CompressGet g(&mgr);
        if (!g.run(base + "/plain", timeoutMs, expected, err))
        {
            jzDebug() << "【FAIL】plain: " << err;
            allOk = false;
        }
        else
        {
            jzDebug() << "【OK】plain 基准载荷长度=" << expected.size();
        }
    }

    // 2) gzip 响应：客户端按 Content-Encoding 自动解压，应与基准一致
    {
        CompressGet g(&mgr);
        JZByteArray body;
        if (!g.run(base + "/gzip", timeoutMs, body, err))
        {
            jzDebug() << "【FAIL】gzip: " << err;
            allOk = false;
        }
        else if (!bytesEqual(body, expected))
        {
            jzDebug() << "【FAIL】gzip 解压后与基准不一致 基准=" << expected.size()
                      << " 解压=" << body.size();
            allOk = false;
        }
        else
        {
            jzDebug() << "【OK】gzip 解压正确 (解压后 " << body.size() << " 字节)";
        }
    }

    // 3) deflate 响应（zlib 封装）：客户端按 Content-Encoding: deflate 自动解压
    {
        CompressGet g(&mgr);
        JZByteArray body;
        if (!g.run(base + "/deflate", timeoutMs, body, err))
        {
            jzDebug() << "【FAIL】deflate: " << err;
            allOk = false;
        }
        else if (!bytesEqual(body, expected))
        {
            jzDebug() << "【FAIL】deflate 解压后与基准不一致 基准=" << expected.size()
                      << " 解压=" << body.size();
            allOk = false;
        }
        else
        {
            jzDebug() << "【OK】deflate 解压正确 (解压后 " << body.size() << " 字节)";
        }
    }

    // 4) /auto 内容协商：客户端现默认发送 Accept-Encoding: gzip, deflate，
    //    服务端应据该头返回 gzip 压缩响应（Content-Encoding: gzip），解压后与基准一致
    {
        CompressGet g(&mgr);
        JZByteArray body;
        if (!g.run(base + "/auto", timeoutMs, body, err))
        {
            jzDebug() << "【FAIL】auto: " << err;
            allOk = false;
        }
        else if (g.encoding() != "gzip")
        {
            jzDebug() << "【FAIL】auto: 期望服务端按 Accept-Encoding 协商返回 gzip，"
                         "实际 Content-Encoding=" << g.encoding();
            allOk = false;
        }
        else if (!bytesEqual(body, expected))
        {
            jzDebug() << "【FAIL】auto: 解压后与基准不一致 基准=" << expected.size()
                      << " 解压=" << body.size();
            allOk = false;
        }
        else
        {
            jzDebug() << "【OK】auto 内容协商生效: 客户端 Accept-Encoding -> 服务端返回 gzip, 解压正确 ("
                      << body.size() << " 字节)";
        }
    }

    jzDebug() << (allOk ? "【结果】PASS: gzip / deflate / plain / auto 解压均正确"
                        : "【结果】FAIL: 存在失败项");
    return allOk ? 0 : 1;
}
