#include <string>
#include <vector>

#include "JZDebug.h"
#include "JZEventLoop.h"
#include "JZTimer.h"
#include "JZThread.h"

#include "JZCrypto.h"
#include "JZNetServer.h"
#include "JZNetClient.h"
#include "JZNetPacket.h"

#include "net_test_common.h"

using namespace std;

// ============ 工具 ============
static JZByteArray fromHex(const std::string &h)
{
    JZByteArray b;
    auto nib = [](char c) -> int
    {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 0;
    };
    for (size_t i = 0; i + 1 < h.size(); i += 2)
        b.push_back((char)(nib(h[i]) * 16 + nib(h[i + 1])));
    return b;
}

static bool bytesEq(const JZByteArray &a, const JZByteArray &b)
{
    if (a.size() != b.size())
        return false;
    if (a.size() == 0)
        return true;
    return memcmp(a.data(), b.data(), a.size()) == 0;
}

// ============ 1. RFC 8439 已知答案测试 (KAT) ============
static void test_cryptoChacha20Poly1305Kat()
{
    JZByteArray key = fromHex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f");
    JZByteArray nonce = fromHex("070000004041424344454647");
    JZByteArray aad = fromHex("50515253c0c1c2c3c4c5c6c7");
    JZByteArray pt = fromHex("4c616469657320616e642047656e746c656d656e206f662074686520636c617373206f66202739393a204966204920636f756c64206f6666657220796f75206f6e6c79206f6e652074697020666f7220746865206675747572652c2073756e73637265656e20776f756c642062652069742e");
    JZByteArray expCt = fromHex("d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d63dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b3692ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc3ff4def08e4b7a9de576d26586cec64b6116");
    JZByteArray expTag = fromHex("1ae10b594f09e26a7e902ecbd0600691");

    JZByteArray ct = JZCrypto::JZChaCha20Poly1305::encrypt(key, nonce, aad, pt);
    if (ct.size() != expCt.size() + 16)
        netReportError("chacha20poly1305 KAT", "密文+tag 长度不符");
    if (!bytesEq(ct.left(expCt.size()), expCt))
        netReportError("chacha20poly1305 KAT", "密文不符(RFC 8439)");
    if (!bytesEq(ct.mid(expCt.size()), expTag))
        netReportError("chacha20poly1305 KAT", "tag 不符(RFC 8439)");
    JZByteArray dec = JZCrypto::JZChaCha20Poly1305::decrypt(key, nonce, aad, ct);
    if (!bytesEq(dec, pt))
        netReportError("chacha20poly1305 KAT", "解密往返不符");

    jzDebug() << "【OK】ChaCha20-Poly1305 RFC 8439 KAT 通过";
}

// ============ 2. 安全属性自测 (往返 / 篡改 / key+nonce 隔离) ============
static void test_cryptoChacha20Poly1305Props()
{
    JZByteArray key = JZCrypto::sha256("key-material-xyz", 15);      // 32 字节
    JZByteArray nonce = JZCrypto::sha256("nonce-material", 13).left(12); // 12 字节
    JZByteArray aad = fromHex("aabbccdd");
    JZByteArray pt = fromHex("00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");

    // 2.1 正常往返
    JZByteArray ct = JZCrypto::JZChaCha20Poly1305::encrypt(key, nonce, aad, pt);
    if (ct.size() != pt.size() + 16)
        netReportError("chacha20poly1305 props", "长度不对(应=明文+16)");
    JZByteArray dec = JZCrypto::JZChaCha20Poly1305::decrypt(key, nonce, aad, ct);
    if (!bytesEq(dec, pt))
        netReportError("chacha20poly1305 props", "往返解密失败");

    // 2.2 密钥错误 -> 校验失败
    JZByteArray wrongKey = JZCrypto::sha256("wrong-key", 9);
    if (JZCrypto::JZChaCha20Poly1305::decrypt(wrongKey, nonce, aad, ct).size() != 0)
        netReportError("chacha20poly1305 props", "错误密钥竟解密成功(认证失效)");

    // 2.3 nonce 错误 -> 校验失败
    JZByteArray wrongNonce = JZCrypto::sha256("wrong-nonce", 11).left(12);
    if (JZCrypto::JZChaCha20Poly1305::decrypt(key, wrongNonce, aad, ct).size() != 0)
        netReportError("chacha20poly1305 props", "错误 nonce 竟解密成功(认证失效)");

    // 2.4 AAD 错误 -> 校验失败
    JZByteArray wrongAad = fromHex("deadbeef");
    if (JZCrypto::JZChaCha20Poly1305::decrypt(key, nonce, wrongAad, ct).size() != 0)
        netReportError("chacha20poly1305 props", "错误 AAD 竟解密成功(认证失效)");

    // 2.5 密文被篡改 1 比特 -> 校验失败
    JZByteArray ctTampered = ct;
    ctTampered[0] = (char)(ctTampered[0] ^ 0x01);
    if (JZCrypto::JZChaCha20Poly1305::decrypt(key, nonce, aad, ctTampered).size() != 0)
        netReportError("chacha20poly1305 props", "篡改密文竟解密成功(完整性失效)");

    // 2.6 tag 被篡改 -> 校验失败
    JZByteArray ctTagTampered = ct;
    ctTagTampered[ctTagTampered.size() - 1] = (char)(ctTagTampered[ctTagTampered.size() - 1] ^ 0x01);
    if (JZCrypto::JZChaCha20Poly1305::decrypt(key, nonce, aad, ctTagTampered).size() != 0)
        netReportError("chacha20poly1305 props", "篡改 tag 竟解密成功(完整性失效)");

    // 2.7 同 key+nonce 两次加密结果一致(确定性); 不同 nonce 必须不同(杜绝密钥流复用)
    JZByteArray ct2 = JZCrypto::JZChaCha20Poly1305::encrypt(key, nonce, aad, pt);
    if (!bytesEq(ct, ct2))
        netReportError("chacha20poly1305 props", "同 key+nonce 加密不确定(实现异常)");
    JZByteArray ct3 = JZCrypto::JZChaCha20Poly1305::encrypt(key, wrongNonce, aad, pt);
    if (bytesEq(ct.left(pt.size()), ct3.left(pt.size())))
        netReportError("chacha20poly1305 props", "不同 nonce 密文相同(密钥流复用!)");

    jzDebug() << "【OK】ChaCha20-Poly1305 安全属性自测通过(认证/篡改检测/nonce 隔离)";
}

// ============ 3. 加密回环测试(套路同 test_netServer.cpp, 仅多一个 token) ============
class NetServerCryptoTest : public JZObject
{
public:
    NetServerCryptoTest(JZEventLoop *loop, int port, const std::string &token)
    {
        if (g_option.enableServer)
        {
            server = new JZNetServer(this);
            server->setToken(token);              // 多加一行: 密码
            server->listen(port);
            connect(&server->sigAccept, this, &NetServerCryptoTest::onAccept);
            connect(&server->sigPacketReceive, this, &NetServerCryptoTest::onServerRecv);
            connect(&server->sigDisconnected, this, &NetServerCryptoTest::onDisconnect);
        }

        if (g_option.enableClient)
        {
            client = new JZNetClient(this);
            client->setToken(token);              // 多加一行: 密码
            client->connectToHost(g_option.host, port);
            if (!client->waitForConnected(3000))
                netReportError("JZNetCrypto", "单线程 client 连接失败(加密握手未建立)");
            connect(&client->sigPacketReceive, this, &NetServerCryptoTest::onClientRecv);
        }

        m_tps.begin();
        if (g_option.enableClient)
            sendNext();
        if (g_option.enableServer && !g_option.enableClient)
            m_bench = serverInstallExit(this, loop, 30000, 20000);
        else
            netInstallExit(this, loop, g_option.loopTime);
        jzDebug() << "==== JZNetCrypto 单线程测试启动 (" << (g_option.enableServer && !g_option.enableClient ? std::string("连接后20s") : std::to_string(g_option.loopTime / 1000) + "s") << " 后退出) ====";
    }

    void onAccept()
    {
        jzDebug() << "[JZNetCrypto] 单线程 accept";
        if (m_bench)
            m_bench->onConnected();
    }

    // Server 收到任意客户端包, 原样回传(全程 AEAD 加密)
    void onServerRecv(uint64_t id, JZNetPackPtr p) { server->sendPacket(id, p.get()); }

    void onDisconnect(uint64_t id)
    {
        netReportError("JZNetCrypto", "单线程 client 异常断开 id=" + std::to_string(id));
    }

    void onClientRecv(JZNetPackPtr p)
    {
        if (p->packetType() != NetPack_echo)
            netReportError("JZNetCrypto", "单线程收到非 echo 包 type=" + std::to_string(p->packetType()));
        m_tps.hit(1);
        sendNext();
    }

    void sendNext()
    {
        JZNetPackEcho echo;
        echo.text = "hello jznet crypto";
        client->sendPacket(&echo);
    }

    JZNetServer *server = nullptr;
    JZNetClient *client = nullptr;
    NetTpsCounter m_tps{"JZNetCrypto 单线程"};
    NetServerBench *m_bench = nullptr;
};

// 多线程客户端线程(各自独立连接), 套路同 test_netServer.cpp, 仅多 setToken
class NetServerCryptoClientThread : public JZThread
{
public:
    NetServerCryptoClientThread(const std::string &host, int port, NetTpsCounter *tps, const std::string &token)
        : m_host(host), m_port(port), m_tps(tps), m_token(token)
    {
        moveToThread(this);
        connect(&sigFinished, this, &NetServerCryptoClientThread::onFinished);
    }

    void onFinished() { moveToThread(m_parent->thread()); }

    void run() override
    {
        JZNetClient *client = new JZNetClient(this);
        client->setToken(m_token);                // 多加一行: 密码
        connect(&client->sigPacketReceive, this, [this, client](JZNetPackPtr p)
        {
            if (p->packetType() != NetPack_echo)
                netReportError("JZNetCrypto", "多线程收到非 echo 包 type=" + std::to_string(p->packetType()));
            m_tps->hit(1);
            JZNetPackEcho echo;
            echo.text = "hello jznet crypto";
            client->sendPacket(&echo);
        });
        connect(&client->sigConnected, this, [this, client]()
        {
            JZNetPackEcho echo;
            echo.text = "hello jznet crypto";
            client->sendPacket(&echo);
        });
        connect(&client->sigDisconnected, this, [this]()
        {
            netReportError("JZNetCrypto", "多线程 client 异常断开");
        });
        client->connectToHost(m_host, m_port);
        exec();
    }

    std::string m_host;
    int m_port;
    NetTpsCounter *m_tps;
    std::string m_token;
};

// 主线程: 1 个 Server + 管理 N 个客户端线程, 套路同 test_netServer.cpp
class NetServerCryptoMultiTest : public JZObject
{
public:
    std::vector<NetServerCryptoClientThread *> threads;
    NetTpsCounter m_tps{"JZNetCrypto 多线程"};

    NetServerCryptoMultiTest(JZEventLoop *loop, int port, const std::string &token)
    {
        if (g_option.enableServer)
        {
            server = new JZNetServer(this);
            server->setToken(token);
            server->listen(port);
            connect(&server->sigAccept, this, &NetServerCryptoMultiTest::onAccept);
            connect(&server->sigPacketReceive, this, &NetServerCryptoMultiTest::onServerRecv);
            connect(&server->sigDisconnected, this, &NetServerCryptoMultiTest::onDisconnect);
        }

        if (g_option.enableClient)
        {
            for (int i = 0; i < NET_TEST_THREAD_COUNT; i++)
            {
                auto t = new NetServerCryptoClientThread(g_option.host, port, &m_tps, token);
                t->setParent(this);
                t->start();
                threads.push_back(t);
            }
        }

        m_tps.begin();
        if (g_option.enableServer && !g_option.enableClient)
            m_bench = serverInstallExit(this, loop, 30000, 20000);
        else
            netInstallExit(this, loop, g_option.loopTime);
        jzDebug() << "==== JZNetCrypto 多线程测试启动 (" << NET_TEST_THREAD_COUNT << " 客户端, " << (g_option.enableServer && !g_option.enableClient ? std::string("连接后20s") : std::to_string(g_option.loopTime / 1000) + "s") << " 后退出) ====";
    }

    void onAccept()
    {
        if (m_bench)
            m_bench->onConnected();
    }

    void stop()
    {
        for (auto t : threads)
        {
            t->quit();
            t->wait();
            delete t;
        }
        threads.clear();
    }

    ~NetServerCryptoMultiTest() { stop(); }

    void onServerRecv(uint64_t id, JZNetPackPtr p) { server->sendPacket(id, p.get()); }
    void onDisconnect(uint64_t /*id*/) {}

    JZNetServer *server = nullptr;
    NetServerBench *m_bench = nullptr;
};

int test_netCrypto_single()
{
    g_netTestActive = true;
    JZEventLoop loop;
    NetServerCryptoTest test(&loop, 18123, "jz-secret-token-123");
    loop.exec();
    double tps = test.m_tps.average();
    netCheckTps("JZNetCrypto 单线程", tps);
    g_netTestActive = false;
    JZThread::msleep(g_option.delayTime);
    return (int)tps;
}

int test_netCrypto_multi()
{
    g_netTestActive = true;
    JZEventLoop loop;
    NetServerCryptoMultiTest test(&loop, 18124, "jz-secret-token-123");
    loop.exec();
    g_netTestActive = false;

    test.stop();
    double tps = test.m_tps.average();
    netCheckTps("JZNetCrypto 多线程", tps);
    JZThread::msleep(g_option.delayTime);
    return (int)tps;
}

int test_netCrypto()
{
    jzDebug() << "########## Net 加密测试: ChaCha20-Poly1305 KAT + 属性 + 加密回环(单/多线程) ##########";
    test_cryptoChacha20Poly1305Kat();
    test_cryptoChacha20Poly1305Props();
    test_netCrypto_single();
    jzDebug() << "【OK】JZNetCrypto 单线程通过";
    test_netCrypto_multi();
    jzDebug() << "【OK】JZNetCrypto 多线程通过";
    return 0;
}
