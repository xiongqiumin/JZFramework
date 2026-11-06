// ---------------------------------------------------------------------------
// test_https_mtls —— HTTPS 双向认证(服务端校验客户端证书/私钥)
//
// 被测链路(JZHttpServer 文档里的第二种用法):
//     server.setSsl(caPem, serverCertPem, serverKeyPem);   // 单向 + 校验客户端
//     client.setSsl("", clientCertPem, clientKeyPem);      // 提供客户端证书
//
// 服务端怎么"验客户端"(本工程实际编的是 MG_TLS=MG_TLS_OPENSSL, 见 JZFramework.vcxproj 的宏):
//   JZHttpServer::listen -> JZNetObject::listen -> 网络线程 JZNetThread::initTls(conn, ssl, host)
//     -> 服务端分支: sslCert/sslKey -> opts.cert/opts.key; **sslCa 非空 -> opts.ca**
//     -> mongoose 的 mg_tls_init(OpenSSL 分支):
//          opts.ca 非空 ⇒ SSL_set_verify(SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT)
//                       + load_ca_certs/add_ca_certs 把 CA 装进信任链
//     -> 握手: 服务端发 CertificateRequest; 客户端用 SSL_use_certificate/PrivateKey 回证书;
//        OpenSSL 验"签发链"与 CertificateVerify(证明持有对应私钥);
//        失败(没交证书 / CA 验不过 / 私钥不符) ⇒ 握手错误, 连接被关
//   => 客户端 JZNetworkReply 拿到 ConnectError。
//
// ⚠ opts.ca 为空 = **单向认证**(不请求客户端证书) —— 那是 test_https.cpp 的场景;
//   本文件专门测"非空"这条路径。
//
// 三个用例(每个用例独立端口, 避免连接复用/残留状态互相影响):
//   1. 客户端证书由受信任 CA 签发    -> 期望 200 + body 原样回显
//   2. 客户端**不带**证书            -> 期望被拒(服务端等不到 Certificate, 直接断)
//   3. 客户端证书由**另一套 CA** 签发 -> 期望被拒(CA 验签失败)
//
// ⚠ 本机若没有这些测试证书也能跑: 证书/私钥直接内嵌在下面(EC P-256, 10 年有效期),
//   与 test_https.cpp 里的服务端自签证书是同一套写法。
// ---------------------------------------------------------------------------
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

#include "JZApplication.h"
#include "JZByteArray.h"
#include "JZDebug.h"
#include "JZEventLoop.h"
#include "JZHttpHandler.h"
#include "JZHttpServer.h"
#include "JZNetworkAccessManager.h"
#include "JZNetworkReply.h"
#include "JZNetworkRequest.h"
#include "JZTimer.h"

using namespace std;

#define LOG(...) do { printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

// ---- 服务端自签证书(CN=localhost, SAN=DNS:localhost,IP:127.0.0.1), 与 test_https.cpp 一致 ----
static const char *kServerCertPem =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBmjCCAT+gAwIBAgIUctFRToPsKrRiJ3ZcGoaBI3dN2r4wCgYIKoZIzj0EAwIw\n"
    "FDESMBAGA1UEAwwJbG9jYWxob3N0MB4XDTI2MDgxOTA0NTcwOFoXDTM2MDgxNjA0\n"
    "NTcwOFowFDESMBAGA1UEAwwJbG9jYWxob3N0MFkwEwYHKoZIzj0CAQYIKoZIzj0D\n"
    "AQcDQgAETI7HVoRolok827xAsBxNkYMjsO4tCVRzUTNobxJZPHrIwa9feLxtHbmV\n"
    "lt8xpccRbEqh7r3Ufc+1pQSwNHttA6NvMG0wHQYDVR0OBBYEFOwXEqOT/hKRT2us\n"
    "lFGDwv4nxX8RMB8GA1UdIwQYMBaAFOwXEqOT/hKRT2uslFGDwv4nxX8RMA8GA1Ud\n"
    "EwEB/wQFMAMBAf8wGgYDVR0RBBMwEYIJbG9jYWxob3N0hwR/AAABMAoGCCqGSM49\n"
    "BAMCA0kAMEYCIQC/c14m4VFNl/jxCO5cdjD8eXQDSGW25HSdaFL2Acz3ewIhALJl\n"
    "klOIfl5lfZfuzVdkXdaabtXUBrXQXN9lMMH0Ikui\n"
    "-----END CERTIFICATE-----\n";

static const char *kServerKeyPem =
    "-----BEGIN EC PRIVATE KEY-----\n"
    "MHcCAQEEIGgYUghIbxtauH9L0y9P8KXdi3D6w16LhjT5jd8iVUhQoAoGCCqGSM49\n"
    "AwEHoUQDQgAETI7HVoRolok827xAsBxNkYMjsO4tCVRzUTNobxJZPHrIwa9feLxt\n"
    "HbmVlt8xpccRbEqh7r3Ufc+1pQSwNHttAw==\n"
    "-----END EC PRIVATE KEY-----\n";

// ---- CA1: 服务端信任它(只信任这一套) ----
static const char *kCaPem =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBkzCCATmgAwIBAgIUPNEhTbGU4CFcrxKVG+64/DW1JYEwCgYIKoZIzj0EAwIw\n"
    "FzEVMBMGA1UEAwwMSlotVGVzdC1DQS0xMB4XDTI2MTEwNjAyNTAyM1oXDTM2MTEw\n"
    "MzAyNTAyM1owFzEVMBMGA1UEAwwMSlotVGVzdC1DQS0xMFkwEwYHKoZIzj0CAQYI\n"
    "KoZIzj0DAQcDQgAEo1DapkrZ0b0Qdym+5PttiMi4ZYfXYjGhbKn3vfc1//iDGk1m\n"
    "O3WYRNoZrNCU3/jCjeZBjK8L6orTUiWWvSwxmaNjMGEwHQYDVR0OBBYEFFl2yjKa\n"
    "nNSm+LbOcAcvcn7pWuRCMB8GA1UdIwQYMBaAFFl2yjKanNSm+LbOcAcvcn7pWuRC\n"
    "MA8GA1UdEwEB/wQFMAMBAf8wDgYDVR0PAQH/BAQDAgEGMAoGCCqGSM49BAMCA0gA\n"
    "MEUCIEoOKV69IJQi3fxcZSYBRDtXgxNB2UCNIJNRVF3JxoddAiEAmqSaq9z5tcjN\n"
    "FXYiO0plYADli6x2w4nWmnVdEXZ/Hi8=\n"
    "-----END CERTIFICATE-----\n";

// ---- 客户端证书 1: 由 CA1 签发(CN=jz-test-client) ----
static const char *kClientCertPem =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBqDCCAU2gAwIBAgIURgHaVjmckQ9hniK1LmzUGT1SKG8wCgYIKoZIzj0EAwIw\n"
    "FzEVMBMGA1UEAwwMSlotVGVzdC1DQS0xMB4XDTI2MTEwNjAyNTAyOFoXDTM2MTEw\n"
    "MzAyNTAyOFowGTEXMBUGA1UEAwwOanotdGVzdC1jbGllbnQwWTATBgcqhkjOPQIB\n"
    "BggqhkjOPQMBBwNCAAT7/IhbyaFEHCuUSu7e4abJ1kTLHCnayADa5a7i930hwh6V\n"
    "pftIdfUKgH2jNDtbgilzk9fU8OUCcdzXhF8cQMRto3UwczAMBgNVHRMBAf8EAjAA\n"
    "MA4GA1UdDwEB/wQEAwIHgDATBgNVHSUEDDAKBggrBgEFBQcDAjAdBgNVHQ4EFgQU\n"
    "jR5CzOYjon5bzQj1HDuoaRnSwIEwHwYDVR0jBBgwFoAUWXbKMpqc1Kb4ts5wBy9y\n"
    "fula5EIwCgYIKoZIzj0EAwIDSQAwRgIhAIAqG4vsNUXk1RewFLiTFjPcVlqOR7+J\n"
    "RIwkb8P4xO7HAiEA5rFnZT4WqQk9QFJi5xdygH0eWxSSZEYEVJ5etBoGhwY=\n"
    "-----END CERTIFICATE-----\n";

static const char *kClientKeyPem =
    "-----BEGIN EC PRIVATE KEY-----\n"
    "MHcCAQEEIKLBRsdJYqsjUfOppzwancVAeOjFPMt+A6YZkEEkgJ81oAoGCCqGSM49\n"
    "AwEHoUQDQgAE+/yIW8mhRBwrlEru3uGmydZEyxwp2sgA2uWu4vd9IcIelaX7SHX1\n"
    "CoB9ozQ7W4Ipc5PX1PDlAnHc14RfHEDEbQ==\n"
    "-----END EC PRIVATE KEY-----\n";

// ---- 客户端证书 2: 由 CA2 签发(服务端不信任) ----
static const char *kOtherClientCertPem =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBqTCCAU+gAwIBAgIUISEdJDuyjGfEdG29kIF6a19XEgkwCgYIKoZIzj0EAwIw\n"
    "FzEVMBMGA1UEAwwMSlotVGVzdC1DQS0yMB4XDTI2MTEwNjAyNTAyOVoXDTM2MTEw\n"
    "MzAyNTAyOVowGzEZMBcGA1UEAwwQanotdGVzdC1jbGllbnQtMjBZMBMGByqGSM49\n"
    "AgEGCCqGSM49AwEHA0IABLuSYXMGGCECAd5+j0O6VD76477sUC9x3q4GWkhmvAji\n"
    "0rhQ7DAdvnxUIrpkh7BY8rexOkwPKIvY3px2E2gecN2jdTBzMAwGA1UdEwEB/wQC\n"
    "MAAwDgYDVR0PAQH/BAQDAgeAMBMGA1UdJQQMMAoGCCsGAQUFBwMCMB0GA1UdDgQW\n"
    "BBSn5ppI2fFjIIVDDsVjNsbSeQye1TAfBgNVHSMEGDAWgBRFd6ykRHpUBN4nFDiD\n"
    "vh2ghpPKPjAKBggqhkjOPQQDAgNIADBFAiA2/LHCoIrYDNB+cebQaVlSDRSlGrDr\n"
    "L2MxfaUul492+wIhAOyHCs4tUgC+PpQ/AYas8TtoDj1pkkzX1xb9NaGnJ+0c\n"
    "-----END CERTIFICATE-----\n";

static const char *kOtherClientKeyPem =
    "-----BEGIN EC PRIVATE KEY-----\n"
    "MHcCAQEEIK5U4htTIXsmWt72sV3QCT/ZcTd0u3ZuzMEtgbopC0QboAoGCCqGSM49\n"
    "AwEHoUQDQgAEu5JhcwYYIQIB3n6PQ7pUPvrjvuxQL3HergZaSGa8COLSuFDsMB2+\n"
    "fFQiumSHsFjyt7E6TA8oi9jenHYTaB5w3Q==\n"
    "-----END EC PRIVATE KEY-----\n";

static atomic<bool> g_failed{false};

class MtlsEchoHandler : public JZHttpHandler
{
public:
    void handle(JZHttpServerClient *client, const JZHttpRequest &req) override
    {
        // 原样回显 body: 用例据此确认"请求真的穿过了双向认证"
        JZByteArray body = req.body();
        JZHttpRespone resp;
        resp.setStatusCode(200);
        resp.setContentType("text/plain; charset=utf-8");
        resp.setBody(string((const char *)body.data(), body.size()));
        client->response(resp, req);
    }
};

// 一个 mTLS 场景: 服务端固定信任 CA1, 客户端证书由参数决定
class MtlsCase : public JZObject
{
public:
    MtlsCase(JZEventLoop *loop, int port, const string &tag, bool expectOk,
             const char *clientCert, const char *clientKey)
        : m_loop(loop), m_tag(tag), m_expectOk(expectOk)
    {
        m_server = new JZHttpServer(this);
        // 服务端: 自己的证书/私钥 + 信任的客户端 CA ⇒ mongoose 侧 is_twoway=true
        m_server->setSsl(kCaPem, kServerCertPem, kServerKeyPem);
        m_server->addRoute("/echo", new MtlsEchoHandler());
        if (!m_server->listen(port))
        {
            fail("服务端 listen 失败(端口被占用?)");
            return;
        }

        m_net = new JZNetworkAccessManager(this);
        JZSSLConfig ssl;
        ssl.sslCa = "";                                  // 不校验服务端自签证书, 本用例只看服务端验客户端
        ssl.sslCert = clientCert ? clientCert : "";
        ssl.sslKey = clientKey ? clientKey : "";
        m_net->setSsl(ssl);

        m_timer = new JZTimer(this);
        m_timer->setSingleShot(true);
        connect(&m_timer->sigTimeout, this, [this]()
        {
            fail("超时 15s 没有结果(握手卡住?)");
        });
        m_timer->start(15 * 1000);

        JZNetworkRequest req("https://localhost:" + to_string(port) + "/echo");
        m_reply = m_net->post(req, str_toUtf8("mtls-ping"));
        connect(&m_reply->sigFinished, this, [this]() { onFinished(); });
    }

    void onFinished()
    {
        const int err = m_reply->error();
        const int code = m_reply->httpStatusCode();
        const string body = str_fromUtf8(m_reply->readAll());

        if (m_expectOk)
        {
            if (err != JZNetworkReply::NoError && err != JZNetworkReply::HttpError)
            {
                fail("期望成功, 实际 " + m_reply->errorString());
                return;
            }
            if (code != 200)
            {
                fail("期望 200, 实际 " + to_string(code));
                return;
            }
            if (body != "mtls-ping")
            {
                fail("body 不匹配: " + body);
                return;
            }
            LOG("[ok] %s: 双向认证通过 (code=200, body=%s)", m_tag.c_str(), body.c_str());
        }
        else
        {
            // 被拒的判定要"硬": 不能是"没回包但也没报错"这种模糊状态。
            if (err == JZNetworkReply::NoError && code == 200)
            {
                fail("期望被拒绝, 但服务端返回 200 —— 客户端证书根本没被校验!");
                return;
            }
            if (err == JZNetworkReply::NoError)
            {
                fail("期望握手失败, 实际拿到了响应 code=" + to_string(code));
                return;
            }
            LOG("[ok] %s: 被拒绝 (%s)", m_tag.c_str(), m_reply->errorString().c_str());
        }

        if (m_timer)
            m_timer->stop();
        m_loop->quit();
    }

    void fail(const string &msg)
    {
        if (!g_failed.exchange(true))
            LOG("[FAIL][%s] %s", m_tag.c_str(), msg.c_str());
        if (m_timer)
            m_timer->stop();
        m_loop->quit();
    }

    JZEventLoop *m_loop;
    string m_tag;
    bool m_expectOk;
    JZHttpServer *m_server = nullptr;
    JZNetworkAccessManager *m_net = nullptr;
    JZNetworkReply *m_reply = nullptr;
    JZTimer *m_timer = nullptr;
};

int main()
{
    JZApplication app;
    LOG("########## HTTPS 双向认证(mTLS): 服务端校验客户端证书 ##########");

    struct CaseDesc
    {
        int port;
        const char *tag;
        bool expectOk;
        const char *cert;
        const char *key;
    };
    // 端口与 test_https(18443/18444)、其它用例错开
    const CaseDesc cases[] = {
        {18450, "客户端证书由受信任 CA 签发", true, kClientCertPem, kClientKeyPem},
        {18451, "客户端不带证书", false, "", ""},
        {18452, "客户端证书由另一套 CA 签发", false, kOtherClientCertPem, kOtherClientKeyPem},
        // 证书对、私钥错: 直接测"服务端验的是不是这把私钥"(CertificateVerify 验不过)
        {18453, "证书与私钥不配对", false, kClientCertPem, kOtherClientKeyPem},
    };

    for (const CaseDesc &c : cases)
    {
        JZEventLoop loop;
        MtlsCase test(&loop, c.port, c.tag, c.expectOk, c.cert, c.key);
        loop.exec();
        if (g_failed.load())
            break;
    }

    if (g_failed.load())
    {
        LOG("########## mTLS 测试失败 ##########");
        return 1;
    }
    LOG("########## mTLS 全部通过 ##########");
    return 0;
}
