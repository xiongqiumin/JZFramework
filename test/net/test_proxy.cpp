// 独立测试: 通过 HTTP 代理访问 http / https 目标。
//  - http  目标: 代理转发(请求行绝对 URI)
//  - https 目标: CONNECT 隧道 + 隧道上 TLS 握手(本次验证的重点)
//
// 用法(在运行前导出环境变量, 与 libcurl 一致):
//   export http_proxy=http://user:pass@host:port
//   export https_proxy=http://user:pass@host:port
//   ./test_proxy
// 也可用命令行覆盖:
//   ./test_proxy --proxy http://user:pass@host:port
//                --http-target  http://example.com
//                --https-target https://example.com
//                --timeout 30000
//
// 退出码: 0=两者都成功, 1=有失败, 2=参数/环境错误。

#include <atomic>
#include <cstdlib>
#include <string>

#include "JZApplication.h"
#include "JZDebug.h"
#include "JZEventLoop.h"
#include "JZTimer.h"
#include "JZNetworkAccessManager.h"
#include "JZNetworkRequest.h"
#include "JZNetworkReply.h"
#include "JZByteArray.h"

// 阻塞式 GET: 在内部事件循环上等待 sigFinished / 超时, 返回结果
class ProxyGet : public JZObject
{
public:
    explicit ProxyGet(JZNetworkAccessManager *mgr) : m_mgr(mgr) {}

    bool run(const std::string &urlStr, int timeoutMs,
             int &code, std::string &body, std::string &err)
    {
        JZEventLoop loop;
        m_loop = &loop;
        m_timeoutMs = timeoutMs;
        m_done = false;
        m_ok = false;
        m_code = 0;
        m_body.clear();
        m_err.clear();

        JZNetworkRequest req(urlStr);
        JZNetworkReply *reply = m_mgr->get(req);
        reply->setFollowRedirects(true);   // 跟随 301/302 重定向(同源)
        m_reply = reply;
        connect(&reply->sigFinished, this, &ProxyGet::onReply);

        JZTimer timer(this);
        timer.setSingleShot(true);
        connect(&timer.sigTimeout, this, &ProxyGet::onTimeout);
        timer.start(timeoutMs);

        loop.exec();

        code = m_code;
        body = m_body;
        err = m_err;
        return m_ok;
    }

protected:
    void onReply()
    {
        JZNetworkReply *reply = m_reply;
        m_done = true;
        if (reply->error() != JZNetworkReply::NoError)
        {
            m_ok = false;
            m_err = reply->errorString();
        }
        else
        {
            m_ok = true;
            m_code = reply->httpStatusCode();
            m_body = str_fromUtf8(reply->readAll());
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
            m_err = "timeout after " + std::to_string(m_timeoutMs) + "ms";
            if (m_loop)
                m_loop->quit();
        }
    }

    JZNetworkAccessManager *m_mgr;
    JZEventLoop *m_loop = nullptr;
    JZNetworkReply *m_reply = nullptr;
    int m_timeoutMs = 30000;
    bool m_done = false;
    bool m_ok = false;
    int m_code = 0;
    std::string m_body;
    std::string m_err;
};

static std::string getEnv(const char *name)
{
#ifdef _WIN32
    char *buf = nullptr;
    size_t len = 0;
    if (_dupenv_s(&buf, &len, name) != 0 || buf == nullptr)
        return std::string();
    std::string v(buf);
    free(buf);
    return v;
#else
    const char *v = std::getenv(name);
    return v ? std::string(v) : std::string();
#endif
}

int main(int argc, char *argv[])
{
    JZApplication a;

    std::string proxyArg;
    std::string httpTarget = "http://www.baidu.com";
    std::string httpsTarget = "https://www.baidu.com";
    httpsTarget = "https://www.doubao.com/";
    int timeoutMs = 30000;

    std::string proxyUrl = "http://127.0.0.1:7001";

    JZProxyConfig pc = JZProxyConfig::fromUrl(proxyUrl);
    if (pc.host.empty())
    {
        jzDebug() << "[错误] 无法解析代理 URL: " << proxyUrl;
        return 2;
    }
    jzDebug() << "[代理] " << pc.host << ":" << pc.port
              << (pc.user.empty() ? " (无认证)" : " (带认证 user=" + pc.user + ")");

    JZNetworkAccessManager mgr;
    JZSSLConfig ssl_config;
    ssl_config.sslSkipVerify = true;
    mgr.setSsl(ssl_config);
    mgr.setProxy(pc);

    bool allOk = true;

    int send_time = 3;
    jzDebug() << "==== 无代理: " << httpTarget << " ====";
    for (int i = 0; i < send_time; i++)
    {
        ProxyGet g(&mgr);
        int code = 0;
        std::string body, err;
        bool ok = g.run(httpTarget, timeoutMs, code, body, err);
        if (ok)
        {
            jzDebug() << "[OK] HTTP 状态码=" << code << " body(前200字节)=\n"
                << body.substr(0, 200);
        }
        else
        {
            jzDebug() << "[失败] HTTP 无代理隧道: " << err;
            allOk = false;
        }
    }

    
    // TLS: 默认跳过证书校验(sslCa 为空), 仅验证代理隧道连通性; 如需校验可传入 CA PEM
    // ---- http 目标经代理转t发 ----
    jzDebug() << "==== [1/2] HTTP 经代理: " << httpTarget << " ====";
    for (int i = 0; i < send_time; i++)
    {
        ProxyGet g(&mgr);
        int code = 0;
        std::string body, err;
        bool ok = g.run(httpTarget, timeoutMs, code, body, err);
        if (ok)
        {
            jzDebug() << "[OK] HTTP 状态码=" << code << " body(前200字节)=\n"
                      << body.substr(0, 200);
        }
        else
        {
            jzDebug() << "[失败] HTTP 经代理: " << err;
            allOk = false;
        }
    }

    // ---- https 目标经代理 CONNECT 隧道 ----
    jzDebug() << "==== [2/2] HTTPS 经代理隧道: " << httpsTarget << " ====";
    for(int i = 0; i < send_time; i++)
    {
        ProxyGet g(&mgr);
        int code = 0;
        std::string body, err;
        bool ok = g.run(httpsTarget, timeoutMs, code, body, err);
        if (ok)
        {
            jzDebug() << "[OK] HTTPS 状态码=" << code << " body(前200字节)=\n"
                      << body.substr(0, 200);
        }
        else
        {
            jzDebug() << "[失败] HTTPS 经代理隧道: " << err;
            allOk = false;
        }
    }

    jzDebug() << (allOk ? "【结果】PASS: HTTP 与 HTTPS 均经代理成功"
                        : "【结果】FAIL: 存在失败项");
    return allOk ? 0 : 1;
}
