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
//   ./test_tcp_bench --mode up   --host 127.0.0.1 --port 18200 -t 15
//   ./test_tcp_bench --mode down --host 192.168.1.5 --port 5201 -t 15
//   ./test_tcp_bench --mode bidir --host 127.0.0.1 --port 18200 -t 15
//
// 配合 iperf3 对测:
//   JZ 做 client 上行:  iperf3 -s            ;  ./test_tcp_bench --mode up --host 127.0.0.1 --port 5201
//   JZ 做 client 下行:  iperf3 -s -R 或 iperf3 -s -i1 ; ./test_tcp_bench --mode down ...

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <cstdlib>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <random>

// iperf 协议 (POSIX 控制握手) 所需头: 实际只用到 htonl/ntohl 等字节序转换
#ifdef _WIN32
#include <winsock2.h>              // 必须早于 windows.h, 故放在本文件最前面的 include 块
#pragma comment(lib, "ws2_32.lib")
#else
#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#endif

#include "JZApplication.h"
#include "JZEventLoop.h"
#include "JZTimer.h"
#include "JZThread.h"
#include "JZByteArray.h"
#include "JZTcpServer.h"
#include "JZTcpSocket.h"
#include "JZUdpSocket.h"
#include "JZDebug.h"
#include "JZCommandLineParser.h"

using namespace std;

// ---------------- 跨平台兼容 ----------------
// Windows 没有 getpid / htobe64, 这里给出等价实现, 使本压测工具在 Win32 下也能编译
#ifdef _WIN32
#include <process.h>
static int benchGetPid() { return (int)_getpid(); }
#else
static int benchGetPid() { return (int)getpid(); }
#endif

// 64 位主机序 -> 网络序(大端)。POSIX 的 htobe64 在 Windows 上不存在, 用 htonl 组合出来。
static uint64_t benchHtobe64(uint64_t v)
{
    uint32_t hi = htonl((uint32_t)(v >> 32));
    uint32_t lo = htonl((uint32_t)(v & 0xFFFFFFFFull));
    return ((uint64_t)lo << 32) | (uint64_t)hi;
}

// ---------------- 命令行与全局配置 ----------------
enum Mode   { ModeUp, ModeDown, ModeBidir };

static Mode  g_mode = ModeBidir;
static string g_host = "127.0.0.1";
static int   g_port = 18200;
static int   g_seconds = 15;
static int   g_chunkSize = 64 * 1024;   // 单块发送字节数
static int   g_sendIntervalMs = 1;      // 上行灌数据定时器间隔
static double g_rateBps = 125000.0;     // 上下行限速(B/s), 默认 1Mb/s = 125000 B/s; 0=不限速
static bool  g_iperfProto = false;      // 是否启用 iperf3 协议(配合 iperf3 -s 对测)
static bool  g_udp = false;             // iperf 协议是否用 UDP 传输

// 限速控制定时器周期(ms): 上行按预算灌数据, 下行按预算读取触发背压
static int   g_throttleMs = 50;

// 发送灌数据定时器间隔: 限速时用较长的 throttle 周期(避免每 tick 只发几百字节), 否则用 --interval
static int txIntervalMs() { return g_rateBps > 0.0 ? g_throttleMs : g_sendIntervalMs; }

static long long nowMs();   // 前向声明(定义在下方)

// 限速预算器: 按 g_rateBps 累计可用字节预算
class RateBudget
{
public:
    void reset(double rateBps)
    {
        m_rateBps = rateBps;
        m_budget = 0.0;
        m_lastMs = nowMs();
    }
    // 返回当前可用预算(字节), 取后清零待 spend
    long long take()
    {
        if (m_rateBps <= 0.0) return LLONG_MAX;   // 不限速
        long long now = nowMs();
        long long dt = now - m_lastMs;
        if (dt > 0)
        {
            m_budget += m_rateBps * dt / 1000.0;
            m_lastMs = now;
            if (m_budget > m_rateBps) m_budget = m_rateBps;   // 预算上限=1秒配额, 防积压
        }
        long long v = (long long)m_budget;
        m_budget -= v;
        if (v < 0) v = 0;
        return v;
    }
    // 预算是否不足(用于判断是否该等待)
    bool exhausted() const { return m_budget <= 0.0; }

private:
    double m_rateBps = 0.0;
    double m_budget = 0.0;
    long long m_lastMs = 0;
};

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
        m_txBudget.reset(g_rateBps);
        m_rxBudget.reset(g_rateBps);
    }

    virtual ~BenchConn() {}

    // 结束后的汇总打印(子类实现)
    virtual void printSummary() = 0;

protected:
    JZEventLoop *m_loop = nullptr;
    JZByteArray m_txData;
    Throughput m_tx, m_rx;
    RateBudget m_txBudget, m_rxBudget;   // 上下行限速预算
    bool m_done = false;
    JZTimer *m_txTimer = nullptr;

    // 发送 n 字节(限速), 返回实际允许发送的字节数
    long long txBudget(long long n)
    {
        long long allow = m_txBudget.take();
        if (allow < 0) allow = 0;
        return n < allow ? n : allow;
    }
    // 接收统计限速: 返回配额内应计入 m_rx 的字节数
    long long rxBudget(long long n)
    {
        long long allow = m_rxBudget.take();
        if (allow < 0) allow = 0;
        return n < allow ? n : allow;
    }

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
        connect(&m_conn->sigConnected, this, &BenchClient::onConnect);
        connect(&m_conn->sigReadyRead, this, &BenchClient::onRead);
        connect(&m_conn->sigDisconnected, this, &BenchClient::onDisconnect);
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

    // 灌数据: 按限速预算决定每 tick 发多少
    void onTxTick()
    {
        long long n = txBudget(g_chunkSize);
        if (n <= 0) return;   // 预算不足, 下个 tick 再发
        writeChunk(n);
    }

    void onRead()
    {
        JZByteArray buf = m_conn->readAll();
        // 下行限速: 只统计配额内的字节数
        m_rx.add(rxBudget(buf.size()));
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
        m_txTimer->start(txIntervalMs());
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
    // 用 <random> 代替 POSIX 的 rand_r, 保证 Windows 下也能编译
    static std::mt19937 gen((unsigned)nowMs() ^ (unsigned)benchGetPid());
    std::uniform_int_distribution<int> dis(0, (int)(sizeof(rndchars) - 2));
    std::string c(IPF_COOKIE_SIZE, '\0');
    for (int i = 0; i < IPF_COOKIE_SIZE - 1; i++)
        c[i] = rndchars[dis(gen)];
    return c;
}

// 构造 iperf 参数 JSON, 并拼接 [uint32 网络序长度][json]
// mode: up=client发(默认), down=reverse(client收), bidir=bidirectional(双工)
// udp: true=UDP传输(JSON 用 udp:true), false=TCP
static JZByteArray ipfBuildParams(int seconds, int blksize, Mode mode, bool udp)
{
    char json[1024];
    const char *transport_field = udp ? "\"udp\":true" : "\"tcp\":true";
    const char *mode_field = "";
    if (mode == ModeDown)
        mode_field = ",\"reverse\":true";
    else if (mode == ModeBidir)
        mode_field = ",\"bidirectional\":true";
    snprintf(json, sizeof(json),
             "{%s,\"omit\":0,\"time\":%d,\"num\":0,\"blockcount\":0,"
             "\"parallel\":1,\"len\":%d%s,\"client_version\":\"3.21\"}",
             transport_field, seconds, blksize, mode_field);
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
        connect(&m_ctrl->sigConnected, this, &IperfClient::onCtrlConnect);
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
                JZByteArray params = ipfBuildParams(g_seconds, g_chunkSize, g_mode, g_udp);
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
        connect(&m_txConn->sigConnected, this, &IperfClient::onTxConnConnect);
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
        connect(&m_rxConn->sigConnected, this, &IperfClient::onRxConnConnect);
        connect(&m_rxConn->sigReadyRead, this, &IperfClient::onRxConnRead);
        connect(&m_rxConn->sigError, this, &IperfClient::onError);
        m_rxConn->connectToHost(g_host, m_port);
        jzDebug() << "[iperf-client] 建立接收流数据连接";
    }

    void onRxConnConnect()
    {
        m_rxConn->write(g_ipfCookie.data(), (int)g_ipfCookie.size());
        // 下行限速: 通过框架 setReadLimit 用 TCP 窗口真实限制对端发送速率
        if (g_rateBps > 0)
            m_rxConn->setReadLimit((int64_t)g_rateBps);
        jzDebug() << "[iperf-client] 接收流已建立, 等待 server 数据 (下行限速 "
                  << (g_rateBps > 0 ? fmtRate(g_rateBps) : "无") << ")";
    }

    void onRxConnRead()
    {
        // 对端已被 setReadLimit 限速, 直接统计实际收到
        m_rx.add(m_rxConn->readAll().size());
    }

    void startTx()
    {
        if (m_txTimer) return;
        m_txTimer = new JZTimer(this);
        connect(&m_txTimer->sigTimeout, this, &IperfClient::onTxTick);
        m_txTimer->start(txIntervalMs());
    }

    void onTxTick()
    {
        long long n = txBudget(g_chunkSize);
        if (n <= 0) return;   // 预算不足, 下个 tick 再发
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

// ---------- iperf3 UDP client (--proto iperf --transport udp) ----------
// 控制连接用 TCP 握手(cookie+JSON udp:true+CREATE_STREAMS), 数据连接用 JZUdpSocket
// 数据报格式(iperf3 64bit): sec(4网络序) + usec(4网络序) + pcount(8网络序) + payload
#define IPF_UDP_CONNECT_MSG   0x36373839U   // "6789" client 发给 server 的 UDP 连接请求
#define IPF_UDP_CONNECT_REPLY 0x39383736U   // "9876" server 回的 UDP 连接确认

class IperfUdpClient : public BenchConn
{
public:
    IperfUdpClient(JZEventLoop *loop, int port)
        : BenchConn(loop), m_port(port)
    {
        // 控制连接(TCP)握手
        m_ctrl = new JZTcpSocket(this);
        connect(&m_ctrl->sigConnected, this, &IperfUdpClient::onCtrlConnect);
        connect(&m_ctrl->sigReadyRead, this, &IperfUdpClient::onCtrlRead);
        connect(&m_ctrl->sigError, this, &IperfUdpClient::onError);
        m_ctrl->connectToHost(g_host, m_port);
        m_tx.start();
        m_rx.start();
        startPrintTimer(this);
        jzDebug() << "[iperf-udp] 控制连接(TCP) " << g_host << ":" << m_port;
    }

    void onCtrlConnect()
    {
        m_ctrl->write(g_ipfCookie.data(), (int)g_ipfCookie.size());
        jzDebug() << "[iperf-udp] 已发 cookie, 等待 server 参数交换";
    }

    void onCtrlRead()
    {
        m_ctrlBuf += str_fromUtf8(m_ctrl->readAll());
        for (size_t i = 0; i < m_ctrlBuf.size(); i++)
        {
            signed char b = (signed char)m_ctrlBuf[i];
            if (b == IPF_STATE_PARAM_EXCHANGE && !m_paramsSent)
            {
                m_paramsSent = true;
                JZByteArray params = ipfBuildParams(g_seconds, g_chunkSize, g_mode, true);  // udp
                m_ctrl->write(params);
                jzDebug() << "[iperf-udp] 已发参数 JSON, 等待 CREATE_STREAMS";
            }
            else if (b == IPF_STATE_CREATE_STREAMS)
            {
                jzDebug() << "[iperf-udp] 收到 CREATE_STREAMS, 建立 UDP 数据流";
                openUdp();
                m_ctrlBuf.clear();
                break;
            }
        }
    }

    void onError(const string &)
    {
        jzDebug() << "[iperf-udp] 控制连接错误";
    }

    virtual void printSummary() override
    {
        jzDebug() << "  client 发送(TX): " << fmtRate(m_tx.finalBps())
                  << "  总 " << (long long)m_tx.total() << "B";
        jzDebug() << "  client 接收(RX): " << fmtRate(m_rx.finalBps())
                  << "  总 " << (long long)m_rx.total() << "B";
    }

    void sendTestEnd()
    {
        if (!m_ctrl) return;
        signed char st = IPF_STATE_TEST_END;
        m_ctrl->write((const char *)&st, 1);
    }

private:
    // 建立 UDP 数据流: bind 本地端口, 发 UDP_CONNECT_MSG 到 server UDP 端口
    void openUdp()
    {
        if (m_udp) return;
        m_udp = new JZUdpSocket(this);
        connect(&m_udp->sigReadyRead, this, &IperfUdpClient::onUdpRead);
        connect(&m_udp->sigError, this, &IperfUdpClient::onError);
        // bind 本地任意端口
        for (int p = 0; p < 200; p++)
        {
            if (m_udp->bind(30000 + p)) { jzDebug() << "[iperf-udp] 本地 UDP 端口 = " << (30000 + p); break; }
        }
        // 发 UDP 连接请求(4字节 magic), server 回 REPLY 后开始收发
        uint32_t msg = htonl(IPF_UDP_CONNECT_MSG);
        m_udp->writeDatagram(g_host, m_port, (const char *)&msg, 4);
        // 下行限速: UDP 通过 setReadLimit 丢弃超额
        if (g_rateBps > 0)
            m_udp->setReadLimit((int64_t)g_rateBps);
        m_udpConnected = true;
        jzDebug() << "[iperf-udp] UDP 数据流已建立, 目标 " << g_host << ":" << m_port
                  << " (下行限速 " << (g_rateBps > 0 ? fmtRate(g_rateBps) : "无") << ")";
        // 上行: 启动发送定时器
        if (g_mode != ModeDown)
            startTx();
    }

    void startTx()
    {
        if (m_txTimer) return;
        m_txTimer = new JZTimer(this);
        connect(&m_txTimer->sigTimeout, this, &IperfUdpClient::onTxTick);
        m_txTimer->start(txIntervalMs());
    }

    // 上行 UDP 发送: 构造 iperf UDP 头 + payload
    void onTxTick()
    {
        long long n = txBudget(g_chunkSize);
        if (n <= 0 || !m_udp || !m_udpConnected) return;
        // 每包一个 iperf 数据报: 头(16B) + 数据(不超过 max 有效载荷)
        while (n > 0)
        {
            JZByteArray pkt;
            uint32_t sec = htonl((uint32_t)time(nullptr));
            uint32_t usec = htonl((uint32_t)(m_useMicro % 1000000));
            m_useMicro += 1000;
            uint64_t pcount = benchHtobe64(++m_pcount);
            pkt.append((const char *)&sec, 4);
            pkt.append((const char *)&usec, 4);
            pkt.append((const char *)&pcount, 8);
            int payload = (int)(n < (long long)(g_chunkSize - 16) ? n : (long long)(g_chunkSize - 16));
            if (payload < 0) payload = 0;
            pkt.append(m_txData.data(), payload);
            m_udp->writeDatagram(g_host, m_port, pkt);
            m_tx.add(pkt.size());
            n -= payload + 16;
        }
    }

    void onUdpRead()
    {
        // 收到数据报
        while (m_udp->hasPendingDatagrams())
        {
            JZUdpDatagram dg = m_udp->readDatagram();
            m_rx.add(dg.data.size());
        }
    }

    int m_port;
    JZTcpSocket *m_ctrl = nullptr;      // 控制连接(TCP)
    JZUdpSocket *m_udp = nullptr;       // UDP 数据连接
    std::string m_ctrlBuf;
    bool m_paramsSent = false;
    bool m_udpConnected = false;
    uint64_t m_pcount = 0;              // iperf UDP 包序号
    uint64_t m_useMicro = 0;            // 伪微秒时间戳
};



// ---------------- main ----------------
static bool parseArgs(int argc, char **argv)
{
    JZCommandLineParser parser;
    parser.setDescription("TCP client 上下行压测 (配合 iperf3/nc 等 Linux 压测工具)");

    std::string mode, protoStr, transportStr;
    parser.addOption("--mode", &mode);
    parser.addOption("--host", &g_host);
    parser.addOption("--port", &g_port);
    parser.addOption(JZCommandLineOption("--time", "-t", "运行秒数", false), &g_seconds);
    parser.addOption("--size", &g_chunkSize);
    parser.addOption("--interval", &g_sendIntervalMs);
    parser.addOption("--rate", &g_rateBps);
    parser.addOption("--proto", &protoStr);
    parser.addOption("--transport", &transportStr);

    if (!parser.parse(argc, argv))
    {
        if (!parser.errorString().empty())
            fprintf(stderr, "参数错误: %s\n", parser.errorString().c_str());
        parser.printHelp();
        return false;
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
    if (parser.isOptionProvided("--transport"))
    {
        if (transportStr == "tcp") g_udp = false;
        else if (transportStr == "udp") g_udp = true;
        else { fprintf(stderr, "未知 --transport '%s' (仅支持 tcp/udp)\n", transportStr.c_str()); return false; }
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

    if (g_iperfProto)
    {
        // iperf3 协议模式: 控制握手 + 数据连接(按 transport 选 TCP/UDP)
        jzDebug() << "==== " << (g_udp ? "UDP" : "TCP") << " client 压测 (iperf3 协议): 对端="
                  << g_host << ":" << g_port << " 时长=" << g_seconds << "s ====";
        g_ipfCookie = ipfMakeCookie();
        if (g_udp)
            bench = new IperfUdpClient(&loop, g_port);
        else
            bench = new IperfClient(&loop, g_port);
    }
    else
    {
        bench = new BenchClient(&loop, g_port);
        jzDebug() << "==== TCP client 压测: 模式="
                  << (g_mode == ModeUp ? "上行(发)" : (g_mode == ModeDown ? "下行(收)" : "上下行"))
                  << " 对端=" << g_host << ":" << g_port
                  << " 时长=" << g_seconds << "s 块=" << g_chunkSize << "B ====";
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
    if (g_iperfProto)
    {
        if (g_udp)
        {
            IperfUdpClient *ic = dynamic_cast<IperfUdpClient *>(bench);
            if (ic) ic->sendTestEnd();
        }
        else
        {
            IperfClient *ic = dynamic_cast<IperfClient *>(bench);
            if (ic) ic->sendTestEnd();
        }
        JZThread::msleep(300);   // 等 server 处理收尾
    }

    // 打印最终结果
    jzDebug() << "==== 结果 (运行 " << (elMs / 1000) << "s) ====";
    bench->printSummary();
    delete bench;
    return 0;
}
