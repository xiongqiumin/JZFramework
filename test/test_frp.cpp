#include <string>
#include <vector>

#include "JZDebug.h"
#include "JZByteArray.h"
#include "JZTimer.h"
#include "JZEventLoop.h"

#include "JZTcpServer.h"
#include "JZTcpSocket.h"
#include "JZFrpServer.h"
#include "JZFrpClient.h"
#include "JZApplication.h"

#include "net/net_test_common.h"

using namespace std;

// ===================== 本地 echo 服务 =====================
// tag 为空时原样回显; 非空时在每行前加 "tag " 前缀(用于区分多实例)
class EchoConn : public JZObject
{
public:
    EchoConn(JZTcpSocket *s, const string &tag, JZObject *parent)
        : JZObject(parent), m_tag(tag)
    {
        m_sock = s;
        connect(&m_sock->sigReadyRead, this, &EchoConn::onRead);
        connect(&m_sock->sigDisconnected, this, &EchoConn::onDisc);
    }

    void onRead()
    {
        m_buf += str_fromUtf8(m_sock->readAll());
        size_t pos;
        while ((pos = m_buf.find('\n')) != string::npos)
        {
            string line = m_buf.substr(0, pos);
            m_buf.erase(0, pos + 1);
            if (m_tag.empty())
                m_sock->write(str_toUtf8(line + "\n"));
            else
                m_sock->write(str_toUtf8(m_tag + " " + line + "\n"));
        }
    }

    void onDisc() { deleteLater(); }

    JZTcpSocket *m_sock;
    string m_buf;
    string m_tag;
};

class EchoServer : public JZObject
{
public:
    EchoServer(int port, const string &tag, JZObject *parent)
        : JZObject(parent), m_tag(tag)
    {
        m_server = new JZTcpServer(this);
        m_server->listen(port);
        connect(&m_server->sigAccept, this, &EchoServer::onAccept);
    }

    void onAccept()
    {
        JZTcpSocket *s = m_server->nextPendingConnection();
        if (s)
            new EchoConn(s, m_tag, this);
    }

    JZTcpServer *m_server;
    string m_tag;
};

// 构造一个压力测试消息: "frp <seq> <len> <len 个 x>"
static string makeStressMsg(uint64_t seq, int len)
{
    return "frp " + to_string(seq) + " " + to_string(len) + " " + string(len, 'x');
}

// 解析 echo 回显行, 校验前缀/序号/长度
static bool parseStressEcho(const string &line, uint64_t &seq, int &len)
{
    string tag = "frp ";
    if (line.compare(0, tag.size(), tag) != 0)
        return false;
    size_t p1 = tag.size();
    size_t p2 = line.find(' ', p1);
    if (p2 == string::npos)
        return false;
    try { seq = stoull(line.substr(p1, p2 - p1)); } catch (...) { return false; }
    size_t p3 = line.find(' ', p2 + 1);
    if (p3 == string::npos)
        return false;
    try { len = stoi(line.substr(p2 + 1, p3 - p2 - 1)); } catch (...) { return false; }
    int payloadLen = (int)(line.size() - (p3 + 1));
    return payloadLen == len;
}

// ===================== 单线程 FRP 隧道回环测试 =====================
// 结构: 本地 echo 服务(7777) <- FRP 客户端(frpc) <- FRP 服务端(frps:8888)
//        <- 远端客户端(连 frps 暴露端口 9999)
// 远端客户端发带序号的消息, 经隧道到达本地 echo 服务原样回传, 校验序号连续性并统计 TPS
class FrpTestSingle : public JZObject
{
public:
    FrpTestSingle(JZEventLoop *loop, int port)
    {
        m_prefix = "frp";

        // 本地 echo 服务
        m_local = new JZTcpServer(this);
        m_local->listen(7777);
        connect(&m_local->sigAccept, this, &FrpTestSingle::onLocalAccept);

        // FRP 服务端
        m_frps = new JZFrpServer(this);
        m_frps->listen(8888);

        // FRP 客户端: 把本地 7777 通过 frps 暴露到 9999
        m_frpcCfg.serverIp = "127.0.0.1";
        m_frpcCfg.serverPort = 8888;
        m_frpcCfg.addProxy("test", "127.0.0.1", 7777, 9999);
        m_frpc = new JZFrpClient();
        m_frpc->setConfig(m_frpcCfg);
        m_frpc->connectToServer();

        // 隧道代理端口就绪后再连远端客户端, 失败则定时重试
        m_connectTimer = new JZTimer(this);
        m_connectTimer->setSingleShot(true);
        connect(&m_connectTimer->sigTimeout, this, &FrpTestSingle::tryConnectRemote);
        m_connectTimer->start(200);

        m_tps.begin();
        netInstallExit(this, loop, 15000);
        jzDebug() << "==== FRP 单线程测试启动 (15s 后退出) ====";
    }

    void tryConnectRemote()
    {
        if (m_remote)
            return;   // 已经连上

        auto *c = new JZTcpSocket(this);
        connect(&c->sigConnected, this, &FrpTestSingle::onRemoteConnect);
        connect(&c->sigReadyRead, this, &FrpTestSingle::onRemoteRead);
        connect(&c->sigError, [this, c](const string &) {
                jzDebug() << "remote connect to proxy failed, retry...";
                c->deleteLater();
                m_remote = nullptr;
                m_connectTimer->start(200);
            });
        c->connectToHost("127.0.0.1", 9999);
    }

    void onLocalAccept()
    {
        m_localSock = m_local->nextPendingConnection();
        if (m_localSock)
            connect(&m_localSock->sigReadyRead, this, &FrpTestSingle::onLocalRead);
    }

    void onLocalRead()
    {
        m_localBuf += str_fromUtf8(m_localSock->readAll());
        size_t pos;
        while ((pos = m_localBuf.find('\n')) != string::npos)
        {
            string line = m_localBuf.substr(0, pos);
            m_localBuf.erase(0, pos + 1);
            m_localSock->write(str_toUtf8(line + "\n"));   // 原样回传
        }
    }

    void onRemoteConnect()
    {
        m_remote = (JZTcpSocket *)sender();
        m_connectTimer->stop();
        sendNext();
    }

    void onRemoteRead()
    {
        m_remoteBuf += str_fromUtf8(m_remote->readAll());
        size_t pos;
        while ((pos = m_remoteBuf.find('\n')) != string::npos)
        {
            string line = m_remoteBuf.substr(0, pos);
            m_remoteBuf.erase(0, pos + 1);

            uint64_t seq;
            if (!netParseMsg(line, m_prefix, seq) || seq != m_expect)
                netReportError("FRP", "校验失败 期望=" + to_string(m_expect) + " 收到=" + line);
            m_expect++;
            m_tps.hit();
            sendNext();
        }
    }

    void sendNext() { m_remote->write(str_toUtf8(netMakeMsg(m_prefix, m_expect))); }

    JZTcpServer *m_local;
    JZTcpSocket *m_localSock = nullptr;
    JZTcpSocket *m_remote = nullptr;
    JZFrpServer *m_frps;
    JZFrpClient *m_frpc;
    JZFrpClientConfig m_frpcCfg;
    JZTimer *m_connectTimer;
    string m_localBuf, m_remoteBuf;
    string m_prefix = "frp";
    uint64_t m_expect = 0;
    NetTpsCounter m_tps{"FRP 单线程"};
};

// ===================== 并发 + 反复断开重连压力测试 =====================
// 覆盖: 多连接并发(多 connectId)、远端快速断开重连、隧道中数据在途时断开、
//       server 端 onFrpcRead 在 socket 已从 m_remoteInfo 移除后触发的崩溃路径。
class StressClient : public JZObject
{
public:
    JZSignal<int> sigDone;

    StressClient(JZEventLoop *loop, int id, int cycles, JZObject *parent)
        : JZObject(parent), sigDone(this), m_loop(loop), m_id(id), m_cycles(cycles)
    {
        m_retry = new JZTimer(this);
        m_retry->setSingleShot(true);
        connect(&m_retry->sigTimeout, this, &StressClient::connectOnce);
        connectOnce();
    }

    void connectOnce()
    {
        m_sock = new JZTcpSocket(this);
        connect(&m_sock->sigConnected, this, &StressClient::onConnect);
        connect(&m_sock->sigReadyRead, this, &StressClient::onRead);
        connect(&m_sock->sigError, this, &StressClient::onError);
        m_sock->connectToHost("127.0.0.1", 9999);
    }

    void onConnect()
    {
        m_seq = 0;
        m_expect = 0;
        m_cycle = 0;
        sendNext();
    }

    void onError(const std::string &)
    {
        if (m_sock) { m_sock->deleteLater(); m_sock = nullptr; }
        m_retry->start(300);   // 代理端口未就绪, 重试
    }

    void sendNext()
    {
        if (m_seq < MsgsPerCycle)
        {
            int len = (m_seq % 5 == 0) ? 65536 : 16;   // 交替发送大包/小包
            m_sock->write(str_toUtf8(makeStressMsg(m_seq, len) + "\n"));
            m_seq++;
        }
    }

    void onRead()
    {
        if (!m_sock)
            return;
        m_buf += str_fromUtf8(m_sock->readAll());
        size_t pos;
        while ((pos = m_buf.find('\n')) != string::npos)
        {
            string line = m_buf.substr(0, pos);
            m_buf.erase(0, pos + 1);

            uint64_t seq; int len;
            if (!parseStressEcho(line, seq, len)
                || seq != (uint64_t)m_expect
                || len != expectedLen((int)seq))
            {
                netReportError("FRP-Stress", "echo mismatch line=" + line);
                return;
            }
            m_expect++;
            sendNext();

            if (m_expect >= MsgsPerCycle)
            {
                // 一个周期完成, 主动断开并进入下一周期(若无更多周期则结束)
                m_sock->disconnectFromHost();
                m_sock->deleteLater();
                m_sock = nullptr;
                m_cycle++;
                if (m_cycle >= m_cycles)
                {
                    sigDone.emit(m_id);
                    return;
                }
                m_retry->start(50);
                return;
            }
        }
    }

    static int expectedLen(int seq) { return (seq % 5 == 0) ? 65536 : 16; }

    JZEventLoop *m_loop;
    int m_id, m_cycles;
    JZTcpSocket *m_sock = nullptr;
    JZTimer *m_retry;
    string m_buf;
    int m_seq = 0, m_expect = 0, m_cycle = 0;

    static const int MsgsPerCycle = 20;
};

class FrpTestStress : public JZObject
{
public:
    FrpTestStress(JZEventLoop *loop, int nClient, int cycles)
        : m_loop(loop), m_total(nClient)
    {
        m_echo = new EchoServer(7777, "", this);   // 原样回显
        m_frps = new JZFrpServer(this);
        m_frps->listen(8888);

        JZFrpClientConfig cc;
        cc.serverIp = "127.0.0.1";
        cc.serverPort = 8888;
        cc.addProxy("s", "127.0.0.1", 7777, 9999);
        m_frpc = new JZFrpClient();
        m_frpc->setConfig(cc);
        m_frpc->connectToServer();

        for (int i = 0; i < nClient; i++)
        {
            auto *c = new StressClient(m_loop, i, cycles, this);
            connect(&c->sigDone, this, &FrpTestStress::onDone);
        }

        jzDebug() << "==== FRP 压力测试: " << nClient << " 并发 x " << cycles << " 周期 ====";
        netInstallExit(this, loop, 30000);
    }

    void onDone(int) { if (++m_done >= m_total) m_loop->quit(); }

    EchoServer *m_echo;
    JZFrpServer *m_frps;
    JZFrpClient *m_frpc;
    JZEventLoop *m_loop;
    int m_done = 0;
    int m_total = 0;   // 在构造函数里按 nClient 设置
};

// ===================== Token 鉴权失败测试 =====================
// frps 设置 token, frpc 使用错误 token, 握手应被拒绝, 隧道端口 9999 不应被监听。
class FrpTestToken : public JZObject
{
public:
    FrpTestToken(JZEventLoop *loop)
    {
        m_frps = new JZFrpServer(this);
        JZFrpServerConfig sc;
        sc.token = "secret";
        m_frps->setConfig(sc);
        m_frps->listen(8888);

        JZFrpClientConfig cc;
        cc.serverIp = "127.0.0.1";
        cc.serverPort = 8888;
        cc.token = "wrong";
        cc.addProxy("t", "127.0.0.1", 7777, 9999);
        m_frpc = new JZFrpClient();
        m_frpc->setConfig(cc);
        m_frpc->connectToServer();

        m_timer = new JZTimer(this);
        m_timer->setSingleShot(true);
        connect(&m_timer->sigTimeout, this, &FrpTestToken::tryConn);
        m_timer->start(300);

        jzDebug() << "==== FRP Token 鉴权失败测试 (6s) ====";
        netInstallExit(this, loop, 6000);
    }

    void tryConn()
    {
        auto *c = new JZTcpSocket(this);
        // 鉴权应已拒绝握手, 任何成功连接都是 FAIL
        connect(&c->sigConnected, [c]() {
                c->deleteLater();
                netReportError("FRP-Token", "鉴权失败却建立了隧道连接");
            });
        connect(&c->sigError, [this, c](const string &) {
                c->deleteLater();
                m_timer->start(300);
            });
        c->connectToHost("127.0.0.1", 9999);
    }

    JZFrpServer *m_frps;
    JZFrpClient *m_frpc;
    JZTimer *m_timer;
};

// ===================== 端口冲突测试 =====================
// 两个 frpc 都试图把本地服务暴露到同一个远端端口 9999,
// server 端跨客户端端口占用检查应拒绝第二个, 远端只能命中第一个(a)。
// 为避免 "谁的 Hand 先到" 的竞态, 先探测确认 c1(7777->9999) 隧道就绪, 再起 c2。
class FrpTestPort : public JZObject
{
public:
    FrpTestPort(JZEventLoop *loop)
    {
        m_echoA = new EchoServer(7777, "A", this);   // 回显 "A ..."
        m_echoB = new EchoServer(7778, "B", this);   // 回显 "B ..."
        m_frps = new JZFrpServer(this);
        m_frps->listen(8888);

        JZFrpClientConfig c1;
        c1.serverIp = "127.0.0.1";
        c1.serverPort = 8888;
        c1.addProxy("a", "127.0.0.1", 7777, 9999);
        m_frpc1 = new JZFrpClient();
        m_frpc1->setConfig(c1);
        m_frpc1->connectToServer();

        // 轮询探测 c1 隧道是否就绪: 连 9999 收到 "A " 即说明 c1 的 Hand 已完成
        m_phase = 0;
        m_probeTimer = new JZTimer(this);
        m_probeTimer->setSingleShot(false);
        connect(&m_probeTimer->sigTimeout, this, &FrpTestPort::probe);
        m_probeTimer->start(150);

        jzDebug() << "==== FRP 端口冲突测试 (8s) ====";
        netInstallExit(this, loop, 8000);
    }

    void probe()
    {
        if (m_phase != 0)
            return;
        auto *c = new JZTcpSocket(this);
        connect(&c->sigConnected, [c]() { c->write(str_toUtf8("ping 0\n")); });
        connect(&c->sigReadyRead, [this, c]() {
            string buf = str_fromUtf8(c->readAll());
            if (buf.find("A ") == string::npos)
            {
                c->deleteLater();
                return;
            }
            c->deleteLater();
            m_phase = 1;
            onC1Ready();
        });
        connect(&c->sigError, [c](const string &) { c->deleteLater(); });
        c->connectToHost("127.0.0.1", 9999);
    }

    void onC1Ready()
    {
        m_probeTimer->stop();
        // c1 已确定占用 9999, 现在起 c2 竞争同一端口, 应被拒绝
        JZFrpClientConfig c2;
        c2.serverIp = "127.0.0.1";
        c2.serverPort = 8888;
        c2.addProxy("b", "127.0.0.1", 7778, 9999);   // 同 remotePort, 应被拒绝
        m_frpc2 = new JZFrpClient();
        m_frpc2->setConfig(c2);
        m_frpc2->connectToServer();

        m_timer = new JZTimer(this);
        m_timer->setSingleShot(true);
        connect(&m_timer->sigTimeout, this, &FrpTestPort::startRemote);
        m_timer->start(500);
    }

    void startRemote()
    {
        m_remote = new JZTcpSocket(this);
        connect(&m_remote->sigConnected, this, &FrpTestPort::onConn);
        connect(&m_remote->sigReadyRead, this, &FrpTestPort::onRead);
        connect(&m_remote->sigError, [this](const string &) {
                if (m_remote) { m_remote->deleteLater(); m_remote = nullptr; }
                m_timer->start(300);
            });
        m_remote->connectToHost("127.0.0.1", 9999);
    }

    void onConn() { m_seq = 0; sendNext(); }

    void sendNext()
    {
        if (!m_remote) return;
        m_remote->write(str_toUtf8("ping " + to_string(m_seq) + "\n"));
        m_seq++;
    }

    void onRead()
    {
        if (!m_remote) return;
        m_buf += str_fromUtf8(m_remote->readAll());
        size_t pos;
        while ((pos = m_buf.find('\n')) != string::npos)
        {
            string line = m_buf.substr(0, pos);
            m_buf.erase(0, pos + 1);
            // 只应命中 A; 出现 B 说明端口冲突未被拒绝(两个 frpc 都注册成功)
            if (line.compare(0, 2, "A ") != 0)
                netReportError("FRP-Port", "端口冲突未被拒绝: " + line);
            if (m_seq >= 20)
            {
                m_remote->disconnectFromHost();
                return;
            }
            sendNext();
        }
    }

    EchoServer *m_echoA, *m_echoB;
    JZFrpServer *m_frps;
    JZFrpClient *m_frpc1, *m_frpc2;
    JZTcpSocket *m_remote = nullptr;
    JZTimer *m_probeTimer = nullptr;
    JZTimer *m_timer = nullptr;
    int m_phase = 0;
    string m_buf;
    int m_seq = 0;
};

int main()
{
    JZApplication a;

    // 1) 基础回环 + TPS
    {
        jzDebug() << "########## FRP 单线程隧道测试 (15s) ##########";
        g_netTestActive = true;
        JZEventLoop loop;
        FrpTestSingle test(&loop, 9999);
        loop.exec();
        double tps = test.m_tps.average();
        netCheckTps("FRP 单线程", tps);
        g_netTestActive = false;
        jzDebug() << "【OK】FRP 单线程通过";
    }

    // 2) 并发 + 反复断开重连压力测试(覆盖崩溃路径)
    {
        g_netTestActive = true;
        JZEventLoop loop;
        FrpTestStress test(&loop, 8, 5);
        loop.exec();
        g_netTestActive = false;
        jzDebug() << "【OK】FRP 压力测试通过";
    }

    // 3) Token 鉴权失败(负向)
    {
        g_netTestActive = true;
        JZEventLoop loop;
        FrpTestToken test(&loop);
        loop.exec();
        g_netTestActive = false;
        jzDebug() << "【OK】FRP Token 鉴权失败测试通过";
    }

    // 4) 端口冲突(负向)
    {
        g_netTestActive = true;
        JZEventLoop loop;
        FrpTestPort test(&loop);
        loop.exec();
        g_netTestActive = false;
        jzDebug() << "【OK】FRP 端口冲突测试通过";
    }

    return 0;
}
