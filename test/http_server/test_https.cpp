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
#include "JZWebSocketServer.h"
#include "JZWebSocket.h"
#include "JZMqttServer.h"
#include "JZMqttClient.h"
#include "JZTcpServer.h"
#include "JZTcpSocket.h"
#include "JZNetServer.h"
#include "JZNetClient.h"
#include "JZNetPacket.h"
#include "JZApplication.h"

using namespace std;

#define LOG(...) do { printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

// 自签 EC 证书 / 私钥 (CN=localhost, SAN=DNS:localhost,IP:127.0.0.1), 仅用于测试
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

static atomic<bool> g_active{true};

static void reportError(const string &where, const string &detail)
{
    if (!g_active.load())
        return;
    jzDebug() << "[FATAL][" << where << "] " << detail;
    exit(1);
}

// ===================== HTTPS 测试 =====================
class HttpsEchoHandler : public JZHttpHandler
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

class HttpsCaTest : public JZObject
{
public:
    HttpsCaTest(JZEventLoop *loop)
        : m_loop(loop)
    {
        int port = 18443;
        m_url = "https://localhost:" + to_string(port);

        m_server = new JZHttpServer(this);
        m_server->setSsl("", kServerCertPem, kServerKeyPem);
        m_server->addRoute("/echo", new HttpsEchoHandler());
        if (!m_server->listen(port))
            reportError("HTTPS_CA", "服务端 listen 失败");
        LOG("[test] HTTPS server listening on %s", m_url.c_str());

        m_net = new JZNetworkAccessManager(this);
        JZSSLConfig server_config;
        server_config.sslCert = kServerCertPem;
        m_net->setSsl(server_config);   // 用服务端自签证书作为 CA 校验

        m_exitTimer = new JZTimer(this);
        m_exitTimer->setSingleShot(true);
        connect(&m_exitTimer->sigTimeout, this, [this]() { m_loop->quit(); });
        m_exitTimer->start(20 * 1000);

        JZNetworkRequest req(m_url + "/echo");
        JZNetworkReply *reply = m_net->post(req, str_toUtf8("ping"));
        connect(&reply->sigFinished, this, [this, reply]() { onFinished(reply); });
    }

    void onFinished(JZNetworkReply *reply)
    {
        if (reply->error() != JZNetworkReply::NoError &&
            reply->error() != JZNetworkReply::HttpError)
        {
            reportError("HTTPS_CA", "net error: " + reply->errorString());
            return;
        }
        int code = reply->httpStatusCode();
        string body = str_fromUtf8(reply->readAll());
        if (code != 200)
            reportError("HTTPS_CA", "状态码错误 期望=200 实际=" + to_string(code));
        if (body != "ping")
            reportError("HTTPS_CA", "body 不匹配 期望=ping 实际=" + body);
        LOG("[test] HTTPS(CA校验) OK: code=%d body=%s", code, body.c_str());
        reply->deleteLater();
        m_loop->quit();
    }

    JZEventLoop *m_loop;
    string m_url;
    JZHttpServer *m_server;
    JZNetworkAccessManager *m_net;
    JZTimer *m_exitTimer;
};

// ===================== WSS 测试 =====================
class WssTest : public JZObject
{
public:
    WssTest(JZEventLoop *loop)
        : m_loop(loop)
    {
        int port = 18444;
        m_url = "wss://localhost:" + to_string(port);

        m_server = new JZWebSocketServer(this);
        m_server->setSsl("", kServerCertPem, kServerKeyPem);
        if (!m_server->listen(port))
            reportError("WSS", "服务端 listen 失败");
        connect(&m_server->sigAccept, this, &WssTest::onServerAccept);
        LOG("[test] WSS server listening on %s", m_url.c_str());

        m_client = new JZWebSocket(this);
        m_client->setSsl(kServerCertPem, "", "");
        connect(&m_client->sigConnected, this, &WssTest::onClientConnect);
        connect(&m_client->sigTextMessageReceived, this, &WssTest::onClientRecv);
        m_client->connectToHost(JZNetworkRequest(m_url));

        m_exitTimer = new JZTimer(this);
        m_exitTimer->setSingleShot(true);
        connect(&m_exitTimer->sigTimeout, this, [this]() { m_loop->quit(); });
        m_exitTimer->start(20 * 1000);
    }

    void onServerAccept()
    {
        m_serverSocket = m_server->nextPendingConnection();
        connect(&m_serverSocket->sigTextMessageReceived, this, &WssTest::onServerRecv);
    }

    void onServerRecv(const string &text)
    {
        if (text != "wss-ping")
            reportError("WSS", "服务端收到非法消息: " + text);
        m_serverSocket->sendTextMessage("wss-pong");   // 回传
    }

    void onClientConnect()
    {
        m_client->sendTextMessage("wss-ping");
    }

    void onClientRecv(const string &text)
    {
        if (text != "wss-pong")
            reportError("WSS", "客户端收到非法消息: " + text);
        LOG("[test] WSS(CA校验) OK: %s", text.c_str());
        m_loop->quit();
    }

    JZEventLoop *m_loop;
    string m_url;
    JZWebSocketServer *m_server;
    JZWebSocket *m_serverSocket = nullptr;
    JZWebSocket *m_client;
    JZTimer *m_exitTimer;
};

// ===================== MQTTS 测试 =====================
class MqttsTest : public JZObject
{
public:
    MqttsTest(JZEventLoop *loop)
        : m_loop(loop)
    {
        int port = 18883;
        m_topic = "tls/test";

        m_server = new JZMqttServer(this);
        m_server->setSsl("", kServerCertPem, kServerKeyPem);
        if (!m_server->listen(port))
            reportError("MQTTS", "服务端 listen 失败");
        LOG("[test] MQTTS server listening on mqtts://localhost:%d", port);

        m_client = new JZMqttClient(this);
        m_client->setSsl(kServerCertPem, "", "");
        connect(&m_client->sigConnected, this, &MqttsTest::onClientConnect);
        connect(&m_client->sigMessage, this, &MqttsTest::onClientMsg);

        JZMqttConnectOpt opt;
        opt.clientId = "tls_test";
        opt.cleanSession = true;
        opt.keepAlive = 30;
        m_client->connectToHost("mqtts://localhost:" + to_string(port), opt);

        m_exitTimer = new JZTimer(this);
        m_exitTimer->setSingleShot(true);
        connect(&m_exitTimer->sigTimeout, this, [this]() { m_loop->quit(); });
        m_exitTimer->start(20 * 1000);
    }

    void onClientConnect()
    {
        JZMqttSubOpt sub;
        sub.topic = m_topic;
        sub.qos = 1;
        m_client->subscribe(sub);

        JZMqttPublishOpt pub;
        pub.topic = m_topic;
        pub.qos = 1;
        pub.payload = str_toUtf8("mqtts-ping");
        m_client->publish(pub);
    }

    void onClientMsg(const JZMqttMessage &msg)
    {
        string payload = str_fromUtf8(msg.payload);
        if (payload != "mqtts-ping")
            reportError("MQTTS", "收到非法消息: " + payload);
        LOG("[test] MQTTS(CA校验) OK: %s", payload.c_str());
        m_loop->quit();
    }

    JZEventLoop *m_loop;
    string m_topic;
    JZMqttServer *m_server;
    JZMqttClient *m_client;
    JZTimer *m_exitTimer;
};

// ===================== 裸 TCP + TLS 测试 =====================
// 验证 mongoose 在裸 TCP 上直接跑 TLS(隐式 TLS) 是否符合标准用法:
// 服务端 accept 后初始化 TLS, 客户端 connect 后初始化 TLS, 上层仍是自定义二进制协议
class TcpSslTest : public JZObject
{
public:
    TcpSslTest(JZEventLoop *loop)
        : m_loop(loop)
    {
        int port = 18999;

        m_server = new JZTcpServer(this);
        m_server->setSsl("", kServerCertPem, kServerKeyPem);   // 服务端证书
        if (!m_server->listen(port))
            reportError("TCP_SSL", "服务端 listen 失败");
        connect(&m_server->sigAccept, this, &TcpSslTest::onServerAccept);
        LOG("[test] TCP+TLS server listening on 127.0.0.1:%d", port);

        m_client = new JZTcpSocket(this);
        m_client->setSsl(kServerCertPem, "", "");   // 用服务端证书作为 CA 校验
        connect(&m_client->sigConnected, this, &TcpSslTest::onClientConnect);
        connect(&m_client->sigReadyRead, this, &TcpSslTest::onClientRead);
        connect(&m_client->sigError, this, &TcpSslTest::onError);
        m_client->connectToHost("127.0.0.1", port);

        m_exitTimer = new JZTimer(this);
        m_exitTimer->setSingleShot(true);
        connect(&m_exitTimer->sigTimeout, this, [this]() { m_loop->quit(); });
        m_exitTimer->start(20 * 1000);
    }

    void onServerAccept()
    {
        m_serverSock = m_server->nextPendingConnection();
        if (m_serverSock)
            connect(&m_serverSock->sigReadyRead, this, &TcpSslTest::onServerRead);
    }

    void onServerRead()
    {
        JZByteArray data = m_serverSock->readAll();
        string msg((const char *)data.data(), data.size());
        if (msg != "tcp-tls-ping")
            reportError("TCP_SSL", "服务端收到非法消息: " + msg);
        m_serverSock->write("tcp-tls-pong", 12);
    }

    void onClientConnect()
    {
        m_client->write("tcp-tls-ping", 12);
    }

    void onClientRead()
    {
        JZByteArray data = m_client->readAll();
        string msg((const char *)data.data(), data.size());
        if (msg != "tcp-tls-pong")
            reportError("TCP_SSL", "客户端收到非法消息: " + msg);
        LOG("[test] TCP+TLS(CA校验) OK: %s", msg.c_str());
        m_loop->quit();
    }

    void onError(const string &err)
    {
        reportError("TCP_SSL", "socket error: " + err);
    }

    JZEventLoop *m_loop;
    JZTcpServer *m_server;
    JZTcpSocket *m_serverSock = nullptr;
    JZTcpSocket *m_client;
    JZTimer *m_exitTimer;
};

// ============== JZNetServer/JZNetClient 裸 TCP + TLS ==============
// 验证自定义协议栈(JZNetPack)也能直接跑在 TLS 之上
class NetSslTest : public JZObject
{
public:
    NetSslTest(JZEventLoop *loop)
        : m_loop(loop)
    {
        int port = 18899;

        m_server = new JZNetServer(this);
        m_server->setSsl("", kServerCertPem, kServerKeyPem);
        connect(&m_server->sigAccept, this, &NetSslTest::onServerAccept);
        connect(&m_server->sigPacketReceive, this, &NetSslTest::onServerPacket);
        if (!m_server->listen(port))
            reportError("NET_SSL", "服务端 listen 失败");
        LOG("[test] NET+TLS server listening on 127.0.0.1:%d", port);

        m_client = new JZNetClient(this);
        m_client->setSsl(kServerCertPem, "", "");   // 用服务端证书作为 CA 校验
        connect(&m_client->sigConnected, this, &NetSslTest::onClientConnect);
        connect(&m_client->sigPacketReceive, this, &NetSslTest::onClientPacket);
        connect(&m_client->sigDisconnected, this, &NetSslTest::onError);
        m_client->connectToHost("127.0.0.1", port);

        m_exitTimer = new JZTimer(this);
        m_exitTimer->setSingleShot(true);
        connect(&m_exitTimer->sigTimeout, this, [this]() { m_loop->quit(); });
        m_exitTimer->start(20 * 1000);
    }

    void onServerAccept()
    {
        // 取出新连接即可, 等待客户端发来的 echo 包
    }

    void onServerPacket(uint64_t id, JZNetPackPtr ptr)
    {
        if (ptr->packetType() != NetPack_echo)
            reportError("NET_SSL", "服务端收到非 echo 包");
        JZNetPackEcho *echo = (JZNetPackEcho *)ptr.get();
        if (echo->text != "net-tls-ping")
            reportError("NET_SSL", "服务端收到非法消息: " + echo->text);
        // 回显
        JZNetPackEcho res;
        res.text = "net-tls-pong";
        m_server->sendPacket(id, &res);
    }

    void onClientConnect()
    {
        JZNetPackEcho echo;
        echo.text = "net-tls-ping";
        m_client->sendPacket(&echo);
    }

    void onClientPacket(JZNetPackPtr ptr)
    {
        if (ptr->packetType() != NetPack_echo)
            reportError("NET_SSL", "客户端收到非 echo 包");
        JZNetPackEcho *echo = (JZNetPackEcho *)ptr.get();
        if (echo->text != "net-tls-pong")
            reportError("NET_SSL", "客户端收到非法消息: " + echo->text);
        LOG("[test] NET+TLS(CA校验) OK: %s", echo->text.c_str());
        m_loop->quit();
    }

    void onError() { reportError("NET_SSL", "client disconnect/error"); }

    JZEventLoop *m_loop;
    JZNetServer *m_server;
    JZNetClient *m_client;
    JZTimer *m_exitTimer;
};

int main()
{
    JZApplication a;
    LOG("########## TLS 证书测试 (HTTPS + WSS + MQTTS) ##########");

    g_active = true;

    // HTTPS
    {
        JZEventLoop loop;
        HttpsCaTest test(&loop);
        loop.exec();
        LOG("【OK】HTTPS 通过");
    }

    // WSS
    {
        JZEventLoop loop;
        WssTest test(&loop);
        loop.exec();
        LOG("【OK】WSS 通过");
    }

    // MQTTS
    {
        JZEventLoop loop;
        MqttsTest test(&loop);
        loop.exec();
        LOG("【OK】MQTTS 通过");
    }

    // 裸 TCP + TLS
    {
        JZEventLoop loop;
        TcpSslTest test(&loop);
        loop.exec();
        LOG("【OK】TCP+TLS 通过");
    }

    // JZNetServer/JZNetClient 裸 TCP + TLS
    {
        JZEventLoop loop;
        NetSslTest test(&loop);
        loop.exec();
        LOG("【OK】NET+TLS 通过");
    }

    LOG("########## TLS 全部通过 ##########");
    return 0;
}
