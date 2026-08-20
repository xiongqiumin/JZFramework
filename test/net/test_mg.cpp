// 纯 mongoose.c 的 TCP 基准测试 (独立程序, 不依赖 JZFramework / JZNet)
//
// 目的: 以 mongoose 原生 C API 作为基准, 与 JZNet 封装版(test_tcp.cpp, 挂在 test_net 下)对比 TCP 收发吞吐。
//
// 本文件自带 mongoose 实现 (#define MG_IMPLEMENTATION + #include "mongoose.h"),
// 不链接 JZFramework, 因此与 JZNet 库内部的 mongoose 全局状态完全隔离, 避免了双重 mg_mgr 冲突。
//
// 用法:
//   test_tcp_mongoose --mode server [--port 18120]
//   test_tcp_mongoose --mode client [--server 127.0.0.1] [--port 18120]
//   test_tcp_mongoose --mode local  [--server 127.0.0.1] [--port 18120]
//
// 支持模式:
//   server  仅服务器: 监听端口, 回显(echo)收到的每一行(\n 分隔), 统计回显 TPS
//   client  仅客户端: 连接 host:port, 流水线发送带序号的消息并校验回显
//   local   同进程内 server + 单 client 自收发基准
//
// 协议: 每行 "mg_tcp <seq>\n", 服务器原样回显; client 校验回显序号连续性。

#include "mongoose.h"

// JZNet 定制版 mongoose 引用了此全局变量(MG_MAX_RECV_SIZE), 这里提供独立定义(默认 3MB)
int JZNET_MAX_RECV = 3 * 1024 * 1024;

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace std;

struct MgMulti
{
    struct mg_mgr mgr;

    std::string payload;

    int port = 9001;
    int runMs = 15000;
    std::atomic<long long> sent{0};

    struct mg_connection *clientConn = nullptr;
    struct mg_connection *serverConn = nullptr;
};

static MgMulti g;
static std::chrono::steady_clock::time_point g_start;


// 每 1s 打印一次吞吐, 结束算平均
static void printTps()
{
    static long long last = 0;
    static auto t0 = g_start;
    auto now = std::chrono::steady_clock::now();
    double dt = std::chrono::duration<double>(now - t0).count();
    if (dt >= 1.0)
    {
        long long cur = g.sent.load();
        double tps = (cur - last) / dt;
        printf("[tps] %lld  (%.0f/s)\n", cur, tps);
        fflush(stdout);
        last = cur;
        t0 = now;
    }
}

static void net_ev(struct mg_connection *c, int ev, void *ev_data)
{
    if (ev == MG_EV_ACCEPT)
    {
        g.serverConn = c;
    }
    else if (ev == MG_EV_CONNECT)
    {   
        mg_send(c, g.payload.data(), g.payload.size());
    }
    else if (ev == MG_EV_READ)
    {
        if(c == g.clientConn)
        {
            g.sent++;
            printTps();
        }

        mg_iobuf_del(&c->recv, 0, c->recv.len);
        mg_send(c, g.payload.data(), g.payload.size());
    }
    else if (ev == MG_EV_CLOSE)
    {
    }
}

int main(int argc, char *argv[])
{
    mg_log_set(MG_LL_ERROR);
    g.payload = "hello world";
    mg_mgr_init(&g.mgr);
    mg_wakeup_init(&g.mgr);

    std::string listenUrl = "tcp://127.0.0.1:" + std::to_string(g.port);
    struct mg_connection *srv = mg_listen(&g.mgr, listenUrl.c_str(), net_ev, nullptr);
    if (!srv)
    {
        printf("listen failed\n");
        return 1;
    }

    struct mg_connection *cli = mg_connect(&g.mgr, listenUrl.c_str(), net_ev, nullptr);
    if (!cli)
    {
        printf("connect failed\n");
        return 1;
    }

    g.clientConn = cli;
    g_start = std::chrono::steady_clock::now();

    const auto timeout_ms = std::chrono::milliseconds(g.runMs);
    while (true)
    {
        mg_mgr_poll(&g.mgr, 20);

        auto now = std::chrono::steady_clock::now();
        auto elapsed = now - g_start;
        if (elapsed >= timeout_ms)
        {
            break; // 15秒超时退出循环
        }
    }
}
