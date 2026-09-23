#include <chrono>
#include <string>
#include <vector>
#include <unordered_set>

#include "JZDebug.h"
#include "JZTimer.h"
#include "JZEventLoop.h"
#include "JZThread.h"
#include "JZByteArray.h"

#include "JZUdpSocket.h"

#include "net_test_common.h"

using namespace std;

// 每轮发送间隔(ms) 与每轮包数: 10ms 发 10 包
static const int UDP_SEND_INTERVAL_MS = 10;
static const int UDP_SEND_BATCH = 10;
// 接收 TPS 达标阈值(>= 500)
static const double UDP_PASS_TPS = 500.0;

// 一个 UDP 回环对: 1 个 Server(bind 固定端口) + 1 个 Client(bind 0 发往 server 端口)
// Client 每 10ms 发送 10 个数据报, Server 原样回显, Client 统计收到的回显(按 seq 去重)
class UdpPair : public JZObject
{
public:
    UdpPair(int serverPort, const string &prefix, NetTpsCounter *recvTps = nullptr, JZEventLoop *loop = nullptr)
        : m_prefix(prefix), m_serverPort(serverPort)
    {
        if (g_option.enableServer)
        {
            m_server = new JZUdpSocket(this);
            m_server->bind(serverPort);
            connect(&m_server->sigReadyRead, this, &UdpPair::onServerRead);
            if (!g_option.enableClient)   // 纯服务器模式: 首次接收即视为连接到达, 之后跑20s
            {
                m_bench = serverInstallExit(this, loop, 30000, 20000);
            }
        }

        if (g_option.enableClient)
        {
            m_client = new JZUdpSocket(this);
            m_client->bind(0);
            connect(&m_client->sigReadyRead, this, &UdpPair::onClientRead);
            connect(&m_client->sigError, this, &UdpPair::onError);
        }

        // 接收 TPS 计数器: 默认自带(单对, 挂到本对象下随析构清理), 多并发时可传入共享计数器
        if (recvTps)
            m_recvTps = recvTps;
        else
        {
            m_ownTps = new NetTpsCounter("UDP 接收 " + prefix, this);
            m_ownTps->begin();
            m_recvTps = m_ownTps;
        }

        if (g_option.enableClient)
        {
            m_sendTimer = new JZTimer(this);
            connect(&m_sendTimer->sigTimeout, this, &UdpPair::onSendTick);
            m_sendTimer->start(UDP_SEND_INTERVAL_MS);
        }
    }

    void onSendTick()
    {
        for (int i = 0; i < UDP_SEND_BATCH; i++)
        {
            m_client->writeDatagram(g_option.host, m_serverPort,
                                    str_toUtf8(netMakeMsg(m_prefix, m_seq)));
            m_seq++;
            m_sent++;
        }
    }

    // Server: 循环读完所有缓存数据报, 原样回显给发送方
    void onServerRead()
    {
        if (m_bench)
            m_bench->onConnected();
        while (m_server->hasPendingDatagrams())
        {
            JZUdpDatagram dgram = m_server->readDatagram();
            m_server->writeDatagram(dgram.peerIp, dgram.peerPort, dgram.data);
            if (g_option.enableServer && !g_option.enableClient)   // 纯服务器模式: 在接收时计时
                m_recvTps->hit();
        }
    }

    // Client: 循环读完所有缓存数据报, 按 seq 去重统计, 并累计每秒接收包数
    void onClientRead()
    {
        while (m_client->hasPendingDatagrams())
        {
            JZUdpDatagram dgram = m_client->readDatagram();
            uint64_t seq;
            if (!netParseMsg(str_fromUtf8(dgram.data), m_prefix, seq))
                continue;   // 忽略非本对的回显(理论不会到达, 防御性跳过)
            if (m_recvSet.insert(seq).second)
                m_recv++;
            if (g_option.enableClient)
                m_recvTps->hit();   // 客户端模式: 统计从服务器收到的包
        }
    }

    void onError(const string &) { netReportError("UDP", "socket 错误"); }

    JZUdpSocket *m_server;
    JZUdpSocket *m_client;
    JZTimer *m_sendTimer = nullptr;
    NetTpsCounter *m_recvTps = nullptr;   // 接收 TPS(共享或自带)
    NetTpsCounter *m_ownTps = nullptr;    // 自带计数器(多并发共享时为空)
    NetServerBench *m_bench = nullptr;
    string m_prefix;
    int m_serverPort = 0;
    uint64_t m_sent = 0;
    uint64_t m_recv = 0;
    uint64_t m_seq = 0;
    unordered_set<uint64_t> m_recvSet;
};

int test_udp_single()
{
    g_netTestActive = true;
    JZEventLoop loop;
    UdpPair pair(18101, "udp", nullptr, &loop);
    if (!(g_option.enableServer && !g_option.enableClient))
        netInstallExit(&pair, &loop, g_option.loopTime);
    jzDebug() << "==== UDP 单线程测试启动 (每 " << UDP_SEND_INTERVAL_MS
              << "ms 发 " << UDP_SEND_BATCH << " 包, " << (g_option.enableServer && !g_option.enableClient ? string("连接后20s") : to_string(g_option.loopTime / 1000) + "s") << " 后退出) ====";
    loop.exec();
    if (g_option.enableServer && !g_option.enableClient)
    {
        // 纯服务器模式: 本机无发送方, 无法统计收发率, 仅校验服务器接收 TPS 达标
        double tps = pair.m_recvTps->average();
        jzDebug() << "[" << "UDP 单线程" << "] 服务器接收总包数=" << pair.m_recvTps->total()
                  << " 平均接收TPS=" << tps << " (阈值 200)";
        if (tps < 200)
            netReportError("UDP 单线程", "服务器接收TPS过低 " + to_string((int)tps) + " < 200");
        jzDebug() << "[Benchmark]["
                  << "UDP 单线程"
                  << "] 接收TPS=" << tps << " OK";
    }
    else
    {
        netCheckTps("UDP 单线程", pair.m_recvTps->average(), UDP_PASS_TPS);
    }
    g_netTestActive = false;
    JZThread::msleep(g_option.delayTime);
    return 0;
}

int test_udp_multi()
{
    g_netTestActive = true;
    JZEventLoop loop;
    // 多并发共享一个接收 TPS 计数器, 每秒打印一次总接收包数
    NetTpsCounter *recvTps = new NetTpsCounter("UDP 多并发 接收");
    recvTps->begin();
    vector<UdpPair *> pairs;
    int basePort = 18106;
    for (int i = 0; i < NET_TEST_THREAD_COUNT; i++)
    {
        auto p = new UdpPair(basePort + i, "udp" + to_string(i), recvTps, &loop);
        pairs.push_back(p);
    }
    // 共享计数器挂到首个 pair 下, pair 析构时一并清理, 避免定时器泄漏到后续测试
    recvTps->setParent(pairs[0]);
    if (!(g_option.enableServer && !g_option.enableClient))
        netInstallExit(pairs[0], &loop, g_option.loopTime);
    jzDebug() << "==== UDP 多并发测试启动 (" << NET_TEST_THREAD_COUNT
              << " 个 UDP 回环对, 每 " << UDP_SEND_INTERVAL_MS << "ms 发 " << UDP_SEND_BATCH
              << " 包, " << (g_option.enableServer && !g_option.enableClient ? string("连接后20s") : to_string(g_option.loopTime / 1000) + "s") << " 后退出) ====";
    loop.exec();

    if (g_option.enableServer && !g_option.enableClient)
    {
        // 纯服务器模式: 本机无发送方, 仅校验服务器接收 TPS 达标
        double tps = recvTps->average();
        jzDebug() << "[" << "UDP 多并发" << "] 服务器接收总包数=" << recvTps->total()
                  << " 平均接收TPS=" << tps << " (阈值 200)";
        if (tps < 200)
            netReportError("UDP 多并发", "服务器接收TPS过低 " + to_string((int)tps) + " < 200");
        jzDebug() << "[Benchmark]["
                  << "UDP 多并发"
                  << "] 接收TPS=" << tps << " OK";
    }
    else
    {
        netCheckTps("UDP 多并发", recvTps->average(), UDP_PASS_TPS);
    }

    // 退出时显式停止并删除所有 pair(含共享计数器/定时器), 防止泄漏到后续测试
    for (auto p : pairs)
        delete p;

    g_netTestActive = false;
    JZThread::msleep(g_option.delayTime);
    return 0;
}

int test_udp()
{
    jzDebug() << "########## UDP 测试 (单线程 + 多并发, 各 " << (NET_TEST_LOOP_MS / 1000) << "s) ##########";
    test_udp_single();
    jzDebug() << "【OK】UDP 单线程通过";
    test_udp_multi();
    jzDebug() << "【OK】UDP 多并发通过";
    return 0;
}
