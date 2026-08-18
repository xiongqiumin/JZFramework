// TCP client 上下行压力测试 (配合 Linux 压测工具 iperf3 / nc 等)
//
// 独立可执行文件, 不参与 test_net 综合自检, 专用于吞吐压测。
//
// 角色:
//   client  默认。连接外部服务器, 按模式灌数据/收数据
//   server  配合外部 client(如 iperf3 -c)对测
// 模式 (仅 client 端有意义; server 端根据客户端行为自动判别统计方向):
//   up      单独上行: client 只发(灌数据), 统计发送吞吐
//   down    单独下行: client 只收(对端灌数据), 统计接收吞吐
//   bidir   上下行:   同时发 + 收, 分别统计发送/接收吞吐
//
// 用法示例:
//   ./test_tcp_bench --role client --mode up   --host 127.0.0.1 --port 18200 -t 15
//   ./test_tcp_bench --role client --mode down --host 192.168.1.5 --port 5201 -t 15
//   ./test_tcp_bench --role client --mode bidir --host 127.0.0.1 --port 18200 -t 15
//   ./test_tcp_bench --role server --port 18200 -t 20   (配合 iperf3 -c 127.0.0.1 -p 18200)
//
// 配合 iperf3 对测:
//   JZ 做 client 上行:  iperf3 -s            ;  ./test_tcp_bench --role client --mode up --host 127.0.0.1 --port 5201
//   JZ 做 client 下行:  iperf3 -s -R 或 iperf3 -s -i1 ; ./test_tcp_bench --role client --mode down ...
//   JZ 做 server 对测:  ./test_tcp_bench --role server --port 5201 ; iperf3 -c 127.0.0.1 -p 5201

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <cstdlib>
#include <cerrno>

// iperf 协议 (POSIX 控制握手) 所需头
#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "JZApplication.h"
#include "JZEventLoop.h"
#include "JZTimer.h"
#include "JZThread.h"
#include "JZByteArray.h"
#include "JZTcpServer.h"
#include "JZTcpSocket.h"
#include "JZDebug.h"
#include "JZCommandLineParser.h"

using namespace std;

// ---------------- 命令行与全局配置 ----------------
enum Role   { RoleClient, RoleServer };
enum Mode   { ModeUp, ModeDown, ModeBidir };

static Role  g_role = RoleClient;
static Mode  g_mode = ModeBidir;
static string g_host = "127.0.0.1";
static int   g_port = 18200;
static int   g_seconds = 15;
static int   g_chunkSize = 64 * 1024;   // 单块发送字节数
static int   g_sendIntervalMs = 1;      // 上行灌数据定时器间隔
static double g_rateBps = 0.0;          // 0=尽力灌, >0 限速(B/s)
static bool  g_iperfProto = false;      // 是否启用 iperf3 协议(配合 iperf3 -s 对测)

static long long nowMs()
{
    return chrono::duration_cast<chrono::milliseconds>(
        chrono::steady_clock::now().time_since_epoch()).count();
}

// ---------------- 吞吐统计 ----------------
struct Throughput
{
    atomic<long long> bytes{0};
    long long beginMs = 0;

    void start() { beginMs = nowMs(); }
    void add(long long n) { bytes += n; }
    // 当前平均速率 B/s
    double rateBps() const
    {
        long long el = nowMs() - beginMs;
        if (el <= 0) return 0.0;
        return (double)bytes.load() / (el / 1000.0);
    }
    // 结束后的整段平均
    double finalBps() const
    {
        long long el = nowMs() - beginMs;
        if (el <= 0) return 0.0;
        return (double)bytes.load() / (el / 1000.0);
    }
    long long total() const { return bytes.load(); }
};

static string fmtRate(double bps)
{
    char buf[64];
    if (bps >= 1024.0 * 1024 * 1024)
        snprintf(buf, sizeof(buf), "%.2f GB/s", bps / (1024.0 * 1024 * 1024));
    else if (bps >= 1024.0 * 1024)
        snprintf(buf, sizeof(buf), "%.2f MB/s", bps / (1024.0 * 1024));
    else if (bps >= 1024.0)
        snprintf(buf, sizeof(buf), "%.2f KB/s", bps / 1024.0);
    else
        snprintf(buf, sizeof(buf), "%.2f B/s", bps);
    return string(buf);
}

// ---------------- 发送/接收对象 ----------------
class BenchConn : public JZObject
{
public:
    BenchConn(JZEventLoop *loop)
        : m_loop(loop)
    {
        m_txData.resize(g_chunkSize, 'x');
    }

    virtual ~BenchConn() {}

    // 结束后的汇总打印(子类实现)
    virtual void printSummary() = 0;

protected:
    JZEventLoop *m_loop = nullptr;
    JZByteArray m_txData;
    Throughput m_tx, m_rx;
    bool m_done = false;
    JZTimer *m_txTimer = nullptr;

    // 统计打印定时器
    void startPrintTimer(JZObject *owner)
    {
        JZTimer *t = new JZTimer(owner);
        connect(&t->sigTimeout, owner, [this]() {
            jzDebug() << "  [TX] " << fmtRate(m_tx.rateBps())
                      << "  [RX] " << fmtRate(m_rx.rateBps())
                      << "  (累计 TX=" << (long long)m_tx.total()
                      << "B RX=" << (long long)m_rx.total() << "B)";
        });
        t->start(1000);
    }
};

// ---------- 客户端 (核心) ----------
class BenchClient : public BenchConn
{
public:
    BenchClient(JZEventLoop *loop, int port)
        : BenchConn(loop)
    {
        m_conn = new JZTcpSocket(this);
        connect(&m_conn->sigConnect, this, &BenchClient::onConnect);
        connect(&m_conn->sigReadyRead, this, &BenchClient::onRead);
        connect(&m_conn->sigDisconnect, this, &BenchClient::onDisconnect);
        connect(&m_conn->sigError, this, &BenchClient::onError);
        m_conn->connectToHost(g_host, port);

        m_rx.start();
        m_tx.start();
        startPrintTimer(this);
    }

    void onConnect()
    {
        jzDebug() << "[client] 已连接 " << g_host << ":" << g_port;
        // 上行/双向: 启动灌数据定时器
        if (g_mode != ModeDown)
            startTxTimer();
    }

    // 灌数据: 根据目标速率决定每 tick 写多少
    void onTxTick()
    {
        long long n = g_chunkSize;
        if (g_rateBps > 0.0)
        {
            // 限速: 每个 tick 应发的字节 = 目标速率 * 间隔秒
            double want = g_rateBps * g_sendIntervalMs / 1000.0;
            n = (long long)want;
            if (n < 1) n = 1;
            if (n > (long long)g_chunkSize) n = g_chunkSize;
        }
        writeChunk(n);
    }

    void onRead()
    {
        JZByteArray buf = m_conn->readAll();
        m_rx.add(buf.size());
    }

    void onDisconnect()
    {
        if (!m_done) jzDebug() << "[client] 连接断开 (对端关闭)";
        finish();
    }

    void onError(const string &)
    {
        jzDebug() << "[client] socket 错误";
        finish();
    }

    virtual void printSummary() override
    {
        jzDebug() << "  client 发送(TX): " << fmtRate(m_tx.finalBps())
                  << "  总 " << (long long)m_tx.total() << "B";
        jzDebug() << "  client 接收(RX): " << fmtRate(m_rx.finalBps())
                  << "  总 " << (long long)m_rx.total() << "B";
    }

private:
    void startTxTimer()
    {
        if (m_txTimer) return;
        m_txTimer = new JZTimer(this);
        connect(&m_txTimer->sigTimeout, this, &BenchClient::onTxTick);
        m_txTimer->start(g_sendIntervalMs);
    }

    // 用固定大小块写满, 不足一块时补零
    void writeChunk(long long n)
    {
        long long written = 0;
        while (written < n)
        {
            int take = (int)((n - written) < g_chunkSize ? (n - written) : g_chunkSize);
            if (!m_conn) return;
            m_conn->write(m_txData.data(), take);
            m_tx.add(take);
            written += take;
        }
    }

    void finish()
    {
        if (m_done) return;
        m_done = true;
        if (m_txTimer) m_txTimer->stop();
    }

    JZTcpSocket *m_conn = nullptr;
};

// ---------------- iperf3 协议兼容 (--proto iperf) ----------------
// 目的: 让 `iperf3 -s` 能识别我们的 JZ client 并统计其发送吞吐。
// 做法: 控制连接与数据连接均使用 JZTcpSocket(纯框架), 走 iperf3 握手状态机。
// 协议(单流 TCP 发送):
//   控制连接: 发 cookie(37B) -> 等 PARAM_EXCHANGE -> 发 JSON 参数 -> 等 CREATE_STREAMS
//   数据连接: 连 server 端口 -> 发 cookie -> 灌数据
#define IPF_COOKIE_SIZE 37
#define IPF_STATE_PARAM_EXCHANGE  9
#define IPF_STATE_CREATE_STREAMS  10
#define IPF_STATE_TEST_END        4

static std::string g_ipfCookie;

// 生成 iperf cookie: 36 个随机字符 + '\0' (共 37 字节)
static std::string ipfMakeCookie()
{
    static const char rndchars[] = "abcdefghijklmnopqrstuvwxyz234567";
    unsigned seed = (unsigned)nowMs() ^ (unsigned)getpid();
    std::string c(IPF_COOKIE_SIZE, '\0');
    for (int i = 0; i < IPF_COOKIE_SIZE - 1; i++)
        c[i] = rndchars[rand_r(&seed) % (sizeof(rndchars) - 1)];
    return c;
}

// 构造 iperf 参数 JSON, 并拼接 [uint32 网络序长度][json]
// mode: up=client发(默认), down=reverse(client收), bidir=bidirectional(双工)
static JZByteArray ipfBuildParams(int seconds, int blksize, Mode mode)
{
    char json[1024];
    const char *mode_field = "";
    if (mode == ModeDown)
        mode_field = ",\"reverse\":true";
    else if (mode == ModeBidir)
        mode_field = ",\"bidirectional\":true";
    snprintf(json, sizeof(json),
             "{\"tcp\":true,\"omit\":0,\"time\":%d,\"num\":0,\"blockcount\":0,"
             "\"parallel\":1,\"len\":%d%s,\"client_version\":\"3.21\"}",
             seconds, blksize, mode_field);
    uint32_t len = htonl((uint32_t)strlen(json));
    JZByteArray out;
    out.append((const char *)&len, sizeof(len));
    out.append(json, (int)strlen(json));
    return out;
}

// iperf 纯框架 client: 控制连接 + 数据连接均用 JZTcpSocket
class IperfClient : public BenchConn
{
public:
    IperfClient(JZEventLoop *loop, int port)
        : BenchConn(loop), m_port(port)
    {
        // 阶段 0: 建立控制连接
        m_ctrl = new JZTcpSocket(this);
        connect(&m_ctrl->sigConnect, this, &IperfClient::onCtrlConnect);
        connect(&m_ctrl->sigReadyRead, this, &IperfClient::onCtrlRead);
        connect(&m_ctrl->sigError, this, &IperfClient::onError);
        m_ctrl->connectToHost(g_host, m_port);
        m_tx.start();
        m_rx.start();
        startPrintTimer(this);
        jzDebug() << "[iperf-client] 控制连接 " << g_host << ":" << m_port;
    }

    // 控制连接建立: 发 cookie
    void onCtrlConnect()
    {
        m_ctrl->write(g_ipfCookie.data(), (int)g_ipfCookie.size());
        jzDebug() << "[iperf-client] 已发 cookie, 等待 server 参数交换";
    }

    // 控制连接收到 server 状态字节: 扫描 CREATE_STREAMS 后建数据连接
    void onCtrlRead()
    {
        m_ctrlBuf += str_fromUtf8(m_ctrl->readAll());
        // server 依次发单字节状态: PARAM_EXCHANGE(9) 之后, 我们发 JSON; 然后 CREATE_STREAMS(10)
        // 收到 9 时发 JSON 参数(若有尚未发送)
        for (size_t i = 0; i < m_ctrlBuf.size(); i++)
        {
            signed char b = (signed char)m_ctrlBuf[i];
            if (b == IPF_STATE_PARAM_EXCHANGE && !m_paramsSent)
            {
                m_paramsSent = true;
                JZByteArray params = ipfBuildParams(g_seconds, g_chunkSize, g_mode);
                m_ctrl->write(params);
                jzDebug() << "[iperf-client] 已发参数 JSON, 等待 CREATE_STREAMS";
            }
            else if (b == IPF_STATE_CREATE_STREAMS)
            {
                jzDebug() << "[iperf-client] 收到 CREATE_STREAMS, 建立数据连接";
                openDataConn();
                m_ctrlBuf.clear();
                break;
            }
            else if (b < 0)
            {
                jzDebug() << "[iperf-client] 收到 server 错误状态 " << (int)b;
            }
        }
    }

    void onError(const string &)
    {
        jzDebug() << "[iperf-client] socket 错误";
    }

    virtual void printSummary() override
    {
        jzDebug() << "  client 发送(TX): " << fmtRate(m_tx.finalBps())
                  << "  总 " << (long long)m_tx.total() << "B";
        jzDebug() << "  client 接收(RX): " << fmtRate(m_rx.finalBps())
                  << "  总 " << (long long)m_rx.total() << "B";
    }

    // 测试结束时: 通过控制连接发 TEST_END, 让 iperf3 server 正常收尾
    void sendTestEnd()
    {
        if (!m_ctrl) return;
        signed char st = IPF_STATE_TEST_END;
        m_ctrl->write((const char *)&st, 1);
        jzDebug() << "[iperf-client] 已发 TEST_END";
    }

private:
    // 建立数据连接: 按模式建发送流/接收流
    void openDataConn()
    {
        if (g_mode == ModeUp)
            openTxConn();
        else if (g_mode == ModeDown)
            openRxConn();
        else  // bidir: 发送流 + 接收流两条
        {
            openTxConn();
            openRxConn();
        }
    }

    // 发送流: client 主动灌数据
    void openTxConn()
    {
        if (m_txConn) return;
        m_txConn = new JZTcpSocket(this);
        connect(&m_txConn->sigConnect, this, &IperfClient::onTxConnConnect);
        connect(&m_txConn->sigReadyRead, this, &IperfClient::onTxConnRead);
        connect(&m_txConn->sigError, this, &IperfClient::onError);
        m_txConn->connectToHost(g_host, m_port);
        jzDebug() << "[iperf-client] 建立发送流数据连接";
    }

    void onTxConnConnect()
    {
        m_txConn->write(g_ipfCookie.data(), (int)g_ipfCookie.size());
        jzDebug() << "[iperf-client] 发送流已建立, 开始灌数据";
        startTx();
    }

    void onTxConnRead()
    {
        m_txConn->readAll();
    }

    // 接收流: 只收 server 发来的数据
    void openRxConn()
    {
        if (m_rxConn) return;
        m_rxConn = new JZTcpSocket(this);
        connect(&m_rxConn->sigConnect, this, &IperfClient::onRxConnConnect);
        connect(&m_rxConn->sigReadyRead, this, &IperfClient::onRxConnRead);
        connect(&m_rxConn->sigError, this, &IperfClient::onError);
        m_rxConn->connectToHost(g_host, m_port);
        jzDebug() << "[iperf-client] 建立接收流数据连接";
    }

    void onRxConnConnect()
    {
        m_rxConn->write(g_ipfCookie.data(), (int)g_ipfCookie.size());
        jzDebug() << "[iperf-client] 接收流已建立, 等待 server 数据";
    }

    void onRxConnRead()
    {
        // 统计从 server 收到的数据
        m_rx.add(m_rxConn->readAll().size());
    }

    void startTx()
    {
        if (m_txTimer) return;
        m_txTimer = new JZTimer(this);
        connect(&m_txTimer->sigTimeout, this, &IperfClient::onTxTick);
        m_txTimer->start(g_sendIntervalMs);
    }

    void onTxTick()
    {
        long long n = g_chunkSize;
        if (g_rateBps > 0.0)
            n = (long long)(g_rateBps * g_sendIntervalMs / 1000.0);
        long long written = 0;
        while (written < n)
        {
            int take = (int)((n - written) < g_chunkSize ? (n - written) : g_chunkSize);
            if (!m_txConn) return;
            m_txConn->write(m_txData.data(), take);
            m_tx.add(take);
            written += take;
        }
    }

    int m_port;
    JZTcpSocket *m_ctrl = nullptr;   // 控制连接
    JZTcpSocket *m_txConn = nullptr; // 发送流数据连接
    JZTcpSocket *m_rxConn = nullptr; // 接收流数据连接(下行/双工)
    std::string m_ctrlBuf;           // 控制连接累积缓冲
    bool m_paramsSent = false;
};

// ---------- 服务器端 (配合外部 client 工具对测) ----------
// 外部 client 连上后:
//   - 外部 client 发数据 (JZ 测下行/接收): 我们 readAll 计数
//   - 外部 client 期望收数据 (JZ 测上行/发送): 我们按模式灌数据
class BenchServer : public BenchConn
{
public:
    BenchServer(JZEventLoop *loop, int port)
        : BenchConn(loop)
    {
        m_server = new JZTcpServer(this);
        m_server->listen(port);
        connect(&m_server->sigAccept, this, &BenchServer::onAccept);
        m_rx.start();
        m_tx.start();
        startPrintTimer(this);
        jzDebug() << "[server] 监听 " << port << " 等待外部 client...";
    }

    void onAccept()
    {
        JZTcpSocket *sock = m_server->nextPendingConnection();
        if (!sock) return;
        jzDebug() << "[server] 有 client 连接进入";
        connect(&sock->sigReadyRead, this, [this, sock]() {
            m_rx.add(sock->readAll().size());
        });
        connect(&sock->sigDisconnect, this, [this, sock]() {
            jzDebug() << "[server] client 断开";
        });
        connect(&sock->sigError, this, [](const string &) {
            jzDebug() << "[server] socket 错误";
        });
        // 上行灌数据(server 主动发), 配合需要数据的外部 client(如 nc 读取)
        m_txTimer = new JZTimer(this);
        m_txData.resize(g_chunkSize, 'y');
        connect(&m_txTimer->sigTimeout, this, [this, sock]() {
            sock->write(m_txData.data(), (int)m_txData.size());
            m_tx.add(m_txData.size());
        });
        m_txTimer->start(g_sendIntervalMs);
    }

    virtual void printSummary() override
    {
        jzDebug() << "  server 发送(TX): " << fmtRate(m_tx.finalBps())
                  << "  总 " << (long long)m_tx.total() << "B";
        jzDebug() << "  server 接收(RX): " << fmtRate(m_rx.finalBps())
                  << "  总 " << (long long)m_rx.total() << "B";
    }

    JZTcpServer *m_server = nullptr;
};

// ---------------- main ----------------
static bool parseArgs(int argc, char **argv)
{
    JZCommandLineParser parser;
    parser.setDescription("TCP client 上下行压测 (配合 iperf3/nc 等 Linux 压测工具)");

    std::string role, mode, protoStr;
    parser.addOption("--role", &role);
    parser.addOption("--mode", &mode);
    parser.addOption("--host", &g_host);
    parser.addOption("--port", &g_port);
    parser.addOption(JZCommandLineOption("--time", "-t", "运行秒数", false), &g_seconds);
    parser.addOption("--size", &g_chunkSize);
    parser.addOption("--interval", &g_sendIntervalMs);
    parser.addOption("--rate", &g_rateBps);
    parser.addOption("--proto", &protoStr);

    if (!parser.parse(argc, argv))
    {
        if (!parser.errorString().empty())
            fprintf(stderr, "参数错误: %s\n", parser.errorString().c_str());
        parser.printHelp();
        return false;
    }

    if (parser.isOptionProvided("--role"))
    {
        if (role == "client") g_role = RoleClient;
        else if (role == "server") g_role = RoleServer;
        else { fprintf(stderr, "未知 --role '%s' (仅支持 client/server)\n", role.c_str()); return false; }
    }
    if (parser.isOptionProvided("--mode"))
    {
        if (mode == "up") g_mode = ModeUp;
        else if (mode == "down") g_mode = ModeDown;
        else if (mode == "bidir") g_mode = ModeBidir;
        else { fprintf(stderr, "未知 --mode '%s' (仅支持 up/down/bidir)\n", mode.c_str()); return false; }
    }
    if (parser.isOptionProvided("--proto"))
    {
        if (protoStr == "iperf") g_iperfProto = true;
        else if (protoStr == "raw") g_iperfProto = false;
        else { fprintf(stderr, "未知 --proto '%s' (仅支持 raw/iperf)\n", protoStr.c_str()); return false; }
    }
    return true;
}

int main(int argc, char **argv)
{
    if (!parseArgs(argc, argv))
    {
        return 1;
    }

    JZApplication app;
    JZEventLoop loop;

    BenchConn *bench = nullptr;

    if (g_role == RoleServer)
    {
        bench = new BenchServer(&loop, g_port);
    }
    else if (g_iperfProto)
    {
        // iperf3 协议模式: 纯 JZTcpSocket 异步握手
        jzDebug() << "==== TCP client 压测 (iperf3 协议): 对端=" << g_host << ":"
                  << g_port << " 时长=" << g_seconds << "s ====";
        g_ipfCookie = ipfMakeCookie();
        bench = new IperfClient(&loop, g_port);
    }
    else
    {
        bench = new BenchClient(&loop, g_port);
        if (g_role == RoleClient)
        {
            jzDebug() << "==== TCP client 压测: 角色=client 模式="
                      << (g_mode == ModeUp ? "上行(发)" : (g_mode == ModeDown ? "下行(收)" : "上下行"))
                      << " 对端=" << g_host << ":" << g_port
                      << " 时长=" << g_seconds << "s 块=" << g_chunkSize << "B ====";
        }
    }

    // 一次性定时器退出事件循环
    JZTimer *quit = new JZTimer(nullptr);
    connect(&quit->sigTimeout, &loop, [&loop]() { loop.quit(); });
    quit->setSingleShot(true);
    quit->start(g_seconds * 1000);

    long long begin = nowMs();
    loop.exec();
    long long elMs = nowMs() - begin;

    // iperf 协议: 发送 TEST_END 让 iperf3 server 正常收尾
    if (g_iperfProto && g_role == RoleClient)
    {
        IperfClient *ic = dynamic_cast<IperfClient *>(bench);
        if (ic) ic->sendTestEnd();
        JZThread::msleep(300);   // 等 server 处理收尾
    }

    // 打印最终结果
    jzDebug() << "==== 结果 (运行 " << (elMs / 1000) << "s) ====";
    bench->printSummary();
    delete bench;
    return 0;
}
