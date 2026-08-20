// 两个线程的 mongoose 基准: 完整模拟 JZNet 的跨线程模型
//   - net 线程  : 跑 mg_mgr_poll(mgr, 20), 负责所有 socket IO (server 回显 + 消费写队列)
//   - app 线程  : 业务线程, 驱动 ping-pong:
//                 收到回显(read 队列, 由 net 线程投递) -> 计一次 -> 把"下一条写"入队 + mg_wakeup 唤醒 net 线程
// 目的: 验证"每条消息都跨线程(读 net->app, 写 app->net) + mg_wakeup 唤醒"这一结构本身的代价
//
// 编译: 见 CMakeLists.txt (test_mg_mulit)
// 用法: ./test_mg_mulit [--time <ms>] [--port <port>] [--nowake] [--poll <ms>]
//   --nowake : 仍然跨线程, 但 app 写后不 mg_wakeup (仅依赖 net 线程 poll 超时兜底)
//   --poll   : net 线程 mg_mgr_poll 的超时(默认 20ms), 改成 0/1 可观察睡眠影响

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <deque>
#include <condition_variable>

#include "mongoose.h"

static const char *kPayload = "hello world\n";
static const size_t kPayloadLen = strlen(kPayload);

// JZNet 定制版 mongoose 引用了此全局变量, 这里提供独立定义(默认 3MB)
int JZNET_MAX_RECV = 3 * 1024 * 1024;

// 模拟 JZNet 的事件对象: 每次写/读都 new 一个, 对面线程 delete(对应 JZNetReqEvent / JZNetResEvent)
struct ReqEvent   // 对应 JZNetReqEvent (Net_Write)
{
    mg_connection *conn = nullptr;
    std::string buffer;   // 对应 req->buffer (JZByteArray 拷贝)
};
struct ResEvent   // 对应 JZNetResEvent (MG_EV_READ)
{
    int event = 0;
    bool isServer = false;
    std::string binData;  // 对应 event->binData
};

struct MgMulti
{
    struct mg_mgr mgr;

    // app -> net : 待发的写请求(每条 new 一个 ReqEvent, 模拟 JZNet 的 postReqEvent new JZNetReqEvent)
    std::mutex writeMtx;
    std::deque<ReqEvent *> writeQ;
    std::condition_variable writeCv;

    // net -> app : 收到的回显(每条 new 一个 ResEvent 投递, 模拟 postEvent new JZNetResEvent)
    std::mutex readMtx;
    std::deque<ResEvent *> readQ;
    std::condition_variable readCv;

    std::atomic<bool> running{true};
    std::atomic<long long> sent{0};

    int port = 9001;
    int runMs = 15000;
    bool useWake = true;     // app 写后是否 mg_wakeup
    int pollMs = 20;         // net 线程 poll 超时

    std::thread netThread;
    std::thread appThread;

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


// net 线程的事件处理: server 回显 + client 收包后 new ResEvent 投递给 app 线程
static void net_ev(struct mg_connection *c, int ev, void *ev_data)
{
    (void)ev_data;
    if (ev == MG_EV_ACCEPT)
    {
        g.serverConn = c;
        return;
    }

    
    ResEvent *e = new ResEvent();                 // 模拟 new JZNetResEvent
    e->event = ev;
    if (ev == MG_EV_READ)
    {
        int n = c->recv.len;
        e->isServer = (g.serverConn == c);
        e->binData.assign((const char *)c->recv.buf, n); // 模拟 event->binData 拷贝
        mg_iobuf_del(&c->recv, 0, n);
    }
    else if (ev == MG_EV_CLOSE)
    {
        c->fn_data = nullptr;
    }

    {
        std::lock_guard<std::mutex> lk(g.readMtx);
        g.readQ.push_back(e);
    }
    g.readCv.notify_one();
}

// net 线程主循环: poll + 消费 app 入队的 ReqEvent(mg_send 后 delete)
static void net_thread_fn()
{
    while (g.running.load())
    {
        mg_mgr_poll(&g.mgr, g.pollMs);

        std::deque<ReqEvent *> batch;
        {
            std::lock_guard<std::mutex> lk(g.writeMtx);
            batch.swap(g.writeQ);
        }
        for (auto *e : batch)
        {
            if (e->conn->is_closing == 0)
                mg_send(e->conn, e->buffer.data(), (int)e->buffer.size());  // 拷贝进内核
            delete e;   // 模拟 JZNetThread 处理完事件后 delete event
        }
    }
}

// app 线程主循环: 等待 ResEvent -> 拷贝解析 -> 计数 -> new ReqEvent 入队 + 唤醒 net 线程
static void app_thread_fn()
{
    while (g.running.load())
    {
        std::unique_lock<std::mutex> lk(g.readMtx);
        g.readCv.wait_for(lk, std::chrono::milliseconds(50), [] { return !g.readQ.empty() || !g.running.load(); });
        if (!g.running.load())
            break;
        if (g.readQ.empty())
            continue;

        ResEvent *re = g.readQ.front();
        g.readQ.pop_front();
        lk.unlock();

        // 首条写请求由 app 线程视角触发(new ReqEvent + 唤醒)
        if(re->event == MG_EV_CONNECT)
        {
            printf("MG_EV_CONNECT\n");
            {
                ReqEvent *we = new ReqEvent();
                we->conn = g.clientConn;
                we->buffer.assign(kPayload, kPayloadLen);
                std::lock_guard<std::mutex> lk(g.writeMtx);
                g.writeQ.push_back(we);
            }
            if (g.useWake)
                mg_wakeup(&g.mgr, 1, nullptr, 0);
        }
        else if(re->event == MG_EV_READ)
        {
            if(!re->isServer)
            {
                g.sent++;
                printTps();
            }

            // 解析拷贝(模拟业务侧 str_fromUtf8 + find/substr/erase)
            std::string buf = std::move(re->binData);

            size_t pos = buf.find('\n');
            if (pos != std::string::npos)
            {
                std::string line = buf.substr(0, pos);
                buf.erase(0, pos + 1);
            }

            // 模拟 JZNet: 业务线程 new 一个 ReqEvent 承载发送缓冲并拷贝, 入队 + 唤醒 net 线程
            ReqEvent *we = new ReqEvent();                        // 模拟 new JZNetReqEvent
            if(!re->isServer)
                we->conn = g.clientConn;
            else
                we->conn = g.serverConn;

            we->buffer.assign(kPayload, kPayloadLen);             // 模拟 req->buffer = JZByteArray(...)
            {
                std::lock_guard<std::mutex> lk2(g.writeMtx);
                g.writeQ.push_back(we);
            }
            if (g.useWake)
            {
                mg_wakeup(&g.mgr, 1, nullptr, 0);
            }
        }
        delete re;   // 模拟业务线程处理完事件后 delete event
    }
}

int main(int argc, char **argv)
{
    mg_log_level = 1;   // 只打印错误
    for (int i = 1; i < argc; i++)
    {
        std::string a = argv[i];
        if (a == "--time" && i + 1 < argc) g.runMs = atoi(argv[++i]);
        else if (a == "--port" && i + 1 < argc) g.port = atoi(argv[++i]);
        else if (a == "--nowake") g.useWake = false;
        else if (a == "--poll" && i + 1 < argc) g.pollMs = atoi(argv[++i]);
    }

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
    g.netThread = std::thread(net_thread_fn);
    g.appThread = std::thread(app_thread_fn);
    
    std::this_thread::sleep_for(std::chrono::milliseconds(g.runMs));
    g.running.store(false);
    g.readCv.notify_all();
    g.writeCv.notify_all();

    g.netThread.join();
    g.appThread.join();

    auto sec = g.runMs / 1000.0;
    long long cnt = g.sent.load();
    printf("[mg_multi %s poll=%dms] TPS = %.0f  (sent=%lld, %.2fs)\n",
           g.useWake ? "wake" : "nowake", g.pollMs, sec > 0 ? cnt / sec : 0.0, cnt, sec);

    mg_mgr_free(&g.mgr);
    return 0;
}
