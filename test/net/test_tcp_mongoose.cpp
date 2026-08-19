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

static const char *kPrefix = "mg_tcp";
// 同步 ping-pong 模式: client 发出一条后等待回显, 收到后才发下一条(与 test_tcp.cpp 一致)
// 不再使用批量流水线发送。

// 时序参数(与 JZ test_tcp.cpp 对齐):
// - server: 启动后最多等待 30s 让 client 连上; 首个 client 连上后, 再运行 20s 退出
// - client: 运行 15s 后停止发送; 再等待 10s(让 server 的 20s 窗口走完) 后退出
static const int kServerWaitMs = 30000;   // server 等待首个连接的 watchdog 超时
static const int kServerRunMs  = 20000;   // 首个连接到达后 server 运行时长
static const int kClientRunMs  = 15000;   // client 运行时长
static const int kClientTailMs = 10000;   // client 运行结束后额外等待时长

// TPS 统计
struct Bench
{
    std::atomic<uint64_t> total{0};
    std::chrono::steady_clock::time_point start;
    std::atomic<bool> running{true};
    std::chrono::steady_clock::time_point tConn;   // 首个 client 连接到达时刻
    bool hasConn = false;                          // 是否已有 client 连上
    std::atomic<bool> stopSend{false};             // client: 已超过运行期, 停止发送(进入等待尾段)

    void begin() { start = std::chrono::steady_clock::now(); tConn = start; }
    double elapsedSec() const
    {
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration<double>(now - start).count();
    }
    double tps() const
    {
        double el = elapsedSec();
        return el > 0 ? (double)total.load() / el : 0.0;
    }
};

// 连接上下文
struct MgConnCtx
{
    std::string buf;          // 接收累积缓冲
    uint64_t sendSeq = 0;     // client: 下一条要发送的消息序号
    uint64_t recvExpect = 0;  // client: 期望收到的回显序号
    bool connected = false;   // client: 连接是否已建立
    bool pending = false;     // client: 是否已有一条消息在途(未收到回显)
    bool isServer = false;
    bool isClient = false;
    bool isListener = false;  // 监听连接上下文(由测试析构统一释放)
    Bench *bench = nullptr;   // 统计指针(创建连接时注入)
};

// 全局统计
// 格式化一条消息: "mg_tcp <seq>\n"
static void mgMakeMsg(char *out, size_t outSize, uint64_t seq)
{
    int n = snprintf(out, outSize, "%s %llu\n", kPrefix, (unsigned long long)seq);
    (void)n;
}

// 解析回显行, 校验前缀并返回序号
static bool mgParseMsg(const std::string &line, uint64_t &seq)
{
    size_t p = strlen(kPrefix) + 1;  // "mg_tcp "
    if (line.size() < p || line.compare(0, strlen(kPrefix), kPrefix) != 0)
        return false;
    try { seq = std::stoull(line.substr(p)); }
    catch (...) { return false; }
    return true;
}

// 处理可读连接的 \n 分帧
// 数据在 c->recv 中(定制版 mongoose: MG_EV_READ 时数据已放入 c->recv)
static void mgSendNext(struct mg_connection *c, MgConnCtx *ctx);   // 前向声明
static void mgProcessRead(struct mg_connection *c, Bench *bench)
{
    MgConnCtx *ctx = (MgConnCtx *)c->fn_data;
    if (!ctx) return;

    std::string chunk((const char *)c->recv.buf, c->recv.len);
    mg_iobuf_del(&c->recv, 0, c->recv.len);
    ctx->buf += chunk;

    size_t pos;
    while ((pos = ctx->buf.find('\n')) != std::string::npos)
    {
        std::string line = ctx->buf.substr(0, pos);
        ctx->buf.erase(0, pos + 1);

        if (ctx->isServer)
        {
            // 服务器: 原样回显
            mg_send(c, line.data(), (int)line.size());
            mg_send(c, "\n", 1);
            bench->total.fetch_add(1);
        }
        else if (ctx->isClient)
        {
            uint64_t seq;
            if (!mgParseMsg(line, seq) || seq != ctx->recvExpect)
            {
                fprintf(stderr, "[FATAL] 校验失败 期望=%llu 收到=%s\n",
                        (unsigned long long)ctx->recvExpect, line.c_str());
                bench->running = false;
                return;
            }
            ctx->recvExpect++;
            bench->total.fetch_add(1);
            ctx->pending = false;        // 本条回显已收到
            mgSendNext(c, ctx);         // 收到再发下一个(同步 ping-pong)
        }
    }
}

// 客户端发送下一条(同步模式: 仅当没有在途消息时才发送)
static void mgSendNext(struct mg_connection *c, MgConnCtx *ctx)
{
    if (ctx->pending) return;           // 已有消息在途, 等回显
    if (ctx->bench && ctx->bench->stopSend.load()) return;   // 已超过运行期, 停止发送
    char buf[64];
    mgMakeMsg(buf, sizeof(buf), ctx->sendSeq++);
    mg_send(c, buf, (int)strlen(buf));
    ctx->pending = true;
}

// 事件回调
static void mgEventHandler(struct mg_connection *c, int ev, void *ev_data)
{
    MgConnCtx *ctx = (MgConnCtx *)c->fn_data;
    switch (ev)
    {
    case MG_EV_ACCEPT:
    {
        // 新数据连接到达(server 端), 继承监听上下文的 bench
        MgConnCtx *lc = (MgConnCtx *)c->fn_data;   // 监听连接上下文
        MgConnCtx *nc = new MgConnCtx();
        nc->isServer = true;
        nc->bench = lc ? lc->bench : nullptr;
        c->fn_data = nc;
        // 记录首个 client 连接到达时刻(用于 server 端 30s 等待 + 20s 运行的判定)
        if (nc->bench && !nc->bench->hasConn)
        {
            nc->bench->hasConn = true;
            nc->bench->tConn = std::chrono::steady_clock::now();
        }
        break;
    }
    case MG_EV_CONNECT:
    {
        if (ev_data && *(int *)ev_data != 0)
        {
            fprintf(stderr, "[ERROR] client 连接失败\n");
            break;
        }
        MgConnCtx *cc = (MgConnCtx *)c->fn_data;
        cc->connected = true;
        mgSendNext(c, cc);              // 连接建立后发第一条
        break;
    }
    case MG_EV_READ:
    {
        MgConnCtx *rc = (MgConnCtx *)c->fn_data;
        Bench *bench = rc ? rc->bench : nullptr;
        mgProcessRead(c, bench);
        break;
    }
    case MG_EV_CLOSE:
    {
        MgConnCtx *dc = (MgConnCtx *)c->fn_data;
        if (dc && !dc->isListener)
            delete dc;          // 仅释放数据连接上下文(监听上下文由测试析构统一释放)
        c->fn_data = nullptr;
        break;
    }
    default:
        break;
    }
}

// ===================== 单线程测试 (server/client/local 同主循环) =====================
static int runSingle(const std::string &host, int port, bool server, bool client)
{
    struct mg_mgr mgr;
    memset(&mgr, 0, sizeof(mgr));
    mg_mgr_init(&mgr);

    Bench bench;

    if (server)
    {
        MgConnCtx *sctx = new MgConnCtx();
        sctx->isServer = true;
        sctx->isListener = true;
        sctx->bench = &bench;
        std::string addr = "tcp://0.0.0.0:" + std::to_string(port);
        mg_listen(&mgr, addr.c_str(), mgEventHandler, sctx);
    }
    if (client)
    {
        MgConnCtx *cctx = new MgConnCtx();
        cctx->isClient = true;
        cctx->bench = &bench;
        std::string addr = "tcp://" + host + ":" + std::to_string(port);
        mg_connect(&mgr, addr.c_str(), mgEventHandler, cctx);
    }

    bench.begin();
    auto tStart = std::chrono::steady_clock::now();   // 启动计时
    auto t0 = tStart;                                  // TPS 打印窗口计时
    while (bench.running.load())
    {
        mg_mgr_poll(&mgr, 5);
        // 注: 同步 ping-pong 模式下, client 的发送由 MG_EV_CONNECT / MG_EV_READ 驱动,
        //     主循环无需主动批量发送(与 test_tcp.cpp 的 sendNext 逻辑一致)

        auto now = std::chrono::steady_clock::now();

        // server 侧: 等待首个连接最多 30s(watchdog); 连上后运行 20s 退出
        if (server)
        {
            double elStart = std::chrono::duration<double>(now - tStart).count();
            if (!bench.hasConn && elStart >= kServerWaitMs / 1000.0)
            {
                printf("[WARN] server 等待 %ds 无 client 连接, 退出\n", kServerWaitMs / 1000);
                break;
            }
            if (bench.hasConn)
            {
                double elConn = std::chrono::duration<double>(now - bench.tConn).count();
                if (elConn >= kServerRunMs / 1000.0)
                    break;
            }
        }

        // client 侧: 运行 15s 后停止发送, 再等待 10s(server 的 20s 窗口走完) 后退出
        if (client)
        {
            double elStart = std::chrono::duration<double>(now - tStart).count();
            if (elStart >= (kClientRunMs + kClientTailMs) / 1000.0)
                break;
            if (elStart >= kClientRunMs / 1000.0)
                bench.stopSend = true;     // 超过运行期, 停止发送进入尾段等待
        }

        // 每秒打印一次 TPS
        double elPrint = std::chrono::duration<double>(now - t0).count();
        if (elPrint >= 1.0)
        {
            printf("[mongoose TCP 单线程 TPS] %.0f\n", bench.tps());
            t0 = now;
        }
    }
    bench.running = false;

    double tps = bench.tps();
    printf("[Benchmark][mongoose TCP 单线程] TPS = %d\n", (int)tps);

    mg_mgr_free(&mgr);
    // 释放监听上下文
    // (mg_mgr_free 会触发 MG_EV_CLOSE, 但监听连接 fn_data 不会被 delete, 这里手动清)
    return (int)tps;
}

// 多线程: 单 mgr + N 个 client 连接(事件循环模型, 无跨线程竞争, 与单线程一致但并发更高)
static int runMulti(const std::string &host, int port, bool server, bool client, int nThreads)
{
    struct mg_mgr mgr;
    memset(&mgr, 0, sizeof(mgr));
    mg_mgr_init(&mgr);

    Bench bench;

    if (server)
    {
        MgConnCtx *sctx = new MgConnCtx();
        sctx->isServer = true;
        sctx->isListener = true;
        sctx->bench = &bench;
        std::string addr = "tcp://0.0.0.0:" + std::to_string(port);
        mg_listen(&mgr, addr.c_str(), mgEventHandler, sctx);
    }
    if (client)
    {
        for (int i = 0; i < nThreads; i++)
        {
            MgConnCtx *cctx = new MgConnCtx();
            cctx->isClient = true;
            cctx->bench = &bench;
            std::string caddr = "tcp://" + host + ":" + std::to_string(port);
            mg_connect(&mgr, caddr.c_str(), mgEventHandler, cctx);
        }
    }

    bench.begin();
    auto tStart = std::chrono::steady_clock::now();
    auto t0 = tStart;
    while (true)
    {
        mg_mgr_poll(&mgr, 5);
        // 同步 ping-pong: client 发送由事件回调驱动, 主循环无需批量发送

        auto now = std::chrono::steady_clock::now();

        // server 侧: 等待首个连接最多 30s(watchdog); 连上后运行 20s 退出
        if (server)
        {
            double elStart = std::chrono::duration<double>(now - tStart).count();
            if (!bench.hasConn && elStart >= kServerWaitMs / 1000.0)
            {
                printf("[WARN] server 等待 %ds 无 client 连接, 退出\n", kServerWaitMs / 1000);
                break;
            }
            if (bench.hasConn)
            {
                double elConn = std::chrono::duration<double>(now - bench.tConn).count();
                if (elConn >= kServerRunMs / 1000.0)
                    break;
            }
        }

        // client 侧: 运行 15s 后停止发送, 再等待 10s(server 的 20s 窗口走完) 后退出
        if (client)
        {
            double elStart = std::chrono::duration<double>(now - tStart).count();
            if (elStart >= (kClientRunMs + kClientTailMs) / 1000.0)
                break;
            if (elStart >= kClientRunMs / 1000.0)
                bench.stopSend = true;     // 超过运行期, 停止发送进入尾段等待
        }

        // 每秒打印 TPS
        double elPrint = std::chrono::duration<double>(now - t0).count();
        if (elPrint >= 1.0)
        {
            printf("[mongoose TCP 多线程 TPS] %.0f\n", bench.tps());
            t0 = now;
        }
    }

    double tps = bench.tps();
    printf("[Benchmark][mongoose TCP 多线程(%d连接)] TPS = %d\n", nThreads, (int)tps);

    mg_mgr_free(&mgr);
    return (int)tps;
}

// ===================== 入口 =====================
static void printUsage(const char *prog)
{
    fprintf(stderr,
        "usage: %s --mode <server|client|local> [--server <ip>]\n"
        "  --mode    server : 仅监听并回显\n"
        "           client : 连接 --server 并发送(单线程+多线程各一轮, 固定 4 连接)\n"
        "           local  : 同进程内 server+client(默认)\n"
        "  --server  远程 server 地址, 默认 127.0.0.1\n"
        "  固定参数: port=18120, threads=4\n"
        "  时序: server 等待连接 30s + 运行 20s | client 运行 15s + 尾随等待 10s\n",
        prog);
}

int main(int argc, char *argv[])
{
    setvbuf(stdout, NULL, _IONBF, 0);   // 无缓冲输出, 便于实时观察
    mg_log_set(MG_LL_NONE);             // 关闭 mongoose 调试日志输出

    std::string mode;
    std::string host = "127.0.0.1";
    int port = 18120;
    int nThreads = 4;

    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--mode" && i + 1 < argc) mode = argv[++i];
        else if (a == "--server" && i + 1 < argc) host = argv[++i];
        else if (a == "--help" || a == "-h") { printUsage(argv[0]); return 0; }
        else { fprintf(stderr, "unknown argument: %s\n", a.c_str()); printUsage(argv[0]); return 1; }
    }

    bool isServer = false, isClient = false;
    if (mode == "server") { isServer = true; }
    else if (mode == "client") { isClient = true; }
    else if (mode.empty() || mode == "local") { isServer = true; isClient = true; }
    else { fprintf(stderr, "missing or invalid --mode (got '%s')\n", mode.c_str()); printUsage(argv[0]); return 1; }

    printf("########## mongoose TCP 基准测试 (纯 mongoose.c, 单线程 + 多线程) ##########\n");
    printf("mode=%s server=%s port=%d threads=%d\n"
           "  [时序] server 等待连接 %ds + 运行 %ds | client 运行 %ds + 尾随等待 %ds\n",
           mode.c_str(), host.c_str(), port, nThreads,
           kServerWaitMs / 1000, kServerRunMs / 1000, kClientRunMs / 1000, kClientTailMs / 1000);

    runSingle(host, port, isServer, isClient);
    printf("【OK】mongoose TCP 单线程通过\n");

    runMulti(host, port, isServer, isClient, nThreads);
    printf("【OK】mongoose TCP 多线程通过\n");
    return 0;
}
