// 两线程 mongoose 基准: 用真实的 JZObject / JZApplication / jzApp->postEvent 事件投递机制,
// 且消息使用 JZNetObject.h 中已定义的 JZNetReqEvent / JZNetResEvent(与真实 JZNet 完全一致)。
// 完整模拟 JZNet 的跨线程事件投递(与 test_mg_mulit.cpp 的区别仅在于: 投递走框架的 postEvent,
// 而不是裸 std::deque + condition_variable)。用来验证 "Qt 风格事件队列 postEvent 比裸队列重" 是否为
// JZNet 比纯 mongoose 慢的主因。
//
// 角色对应:
//   NetObj (继承 JZThread, 独立线程)  ~ JZNetThread: 跑 mg_mgr_poll, event() 处理 JZNetReqEvent(Net_Write) 并 mg_send
//   AppObj (继承 JZObject, 主线程)    ~ JZNetObject: event() 处理 JZNetResEvent(读数据回显), 计数, new JZNetReqEvent 投递 + 唤醒
//   server 连接的 MG_EV_READ          ~ JZNetObject::onClientRead: new JZNetResEvent + jzApp->postEvent(appObj, res)
//
// 编译: 见 CMakeLists.txt (test_mg_postevent)
// 用法: ./test_mg_postevent [--time <ms>] [--port <port>] [--poll <ms>]

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include "JZApplication.h"
#include "JZThread.h"
#include "JZTimer.h"
#include "JZNet/JZNetObject.h"
#include "mongoose.h"
#include "JZDebug.h"

using namespace std;

// JZNet 定制版 mongoose 引用了此全局变量(已在 JZFramework 中定义, 这里仅声明)
extern int JZNET_MAX_RECV;

// 与 test_jz_tcp_cpp 一致: 1KB 负载 + 行尾 '\n' 分包
static const int kPayloadLen = 1024;
static std::string g_payload;   // 初始化在 main

static std::atomic<long long> g_sent{0};
static std::atomic<bool> g_running{true};
static std::atomic<bool> g_posting{true};   // 关闭连接期间禁止再 postEvent(jzApp 即将析构)
static int g_pollMs = 20;
// 模拟 JZNet 的 net 线程开销: 全局互斥锁 + 连接 id -> mg_connection* 的 std::map 查找
// (--heavy 开启, 复刻 JZNetThread::mg_thread_fn 的 m_objectMutex + m_connMap.find 成本)
static bool g_heavy = false;
static std::mutex g_netMutex;

class NetObj;
class MySockServer;
// 全局指针, 供 mongoose 回调使用(仅基准用途)
static NetObj *g_net = nullptr;
static MySockServer *g_sock_server = nullptr;

static std::map<uint64_t, mg_connection *> g_connMap;   // conn id -> connection
map<uint64_t,JZObject*> g_newObjectMap;
uint64_t g_clientConn = 0;
uint64_t g_clientServerConn = 0;

void bindConn(mg_connection *c, JZObject *object)
{
    g_connMap[c->id] = c;
    g_newObjectMap[c->id] = object;
}

// NetObj: 独立线程跑 mg_mgr_poll, 通过 event() 接收跨线程投递的 ReqEvent
class NetObj : public JZThread
{
    JZ_OBJECT_DECLARE(NetObj)
public:
    mg_mgr m_mgr;
    mg_connection *clientConn = nullptr;

    NetObj() : JZThread(nullptr)
    {
        moveToThread(this);          // 自身事件在自己的线程处理
        mg_log_set(MG_LL_ERROR);
        mg_mgr_init(&m_mgr);
        mg_wakeup_init(&m_mgr);
    }
    ~NetObj() {}   // m_mgr 由 main 在 jzApp 存活期间显式释放, 避免析构时 postEvent 崩溃

    void wakeUpNet() { mg_wakeup(&m_mgr, 1, nullptr, 0); }

protected:
    virtual void run() override
    {
        while (g_running.load())
        {
            mg_mgr_poll(&m_mgr, g_pollMs);
            processEvents();          // 处理 postEvent 过来的 ReqEvent
        }
        moveToThread(jzApp->mainThread());
    }

    virtual bool event(JZEvent *e) override
    {
        if (e->type() == JZEvent_netReq)
        {
            JZNetReqEvent *req = (JZNetReqEvent *)e;
            if (req->netCmd == JZNetReqEvent::Net_Write)
            {
                mg_connection *conn = g_connMap[req->conn];
                if (conn && conn->is_closing == 0)
                {
                    //jzDebug() << "mg_send" << conn;
                    mg_send(conn, req->buffer.data(), (int)req->buffer.size());
                }
            }
            return true;
        }
        return JZThread::event(e);
    }
};
JZ_OBJECT_IMPL(NetObj, JZThread)

class MySockClient : public JZObject
{
public:
    MySockClient()
    {
    }

    bool event(JZEvent *event) override
    {
        if (event->type() == JZEvent_netRes) 
        {
            JZNetResEvent* net_event = (JZNetResEvent*)event;
            onNetEvent(net_event);
        }
        return true;
    }

    void onNetEvent(JZNetResEvent *event)
    {
        if (m_conn == INVALID_NET_HANDLE)
            return;
        if (m_conn != event->conn && event->event == JZ_NET_EV_CLOSE)
            return;

        if(m_conn != event->conn)
        {
            jzDebug() << "onNetEvent" << this << m_conn  << "evt" << event->conn;
            JZ_ASSERT(m_conn == event->conn);
        }

        int ev = event->event;
        if (ev == JZ_NET_EV_CONNECT)
        {
            jzDebug() << "JZ_NET_EV_CONNECT" << m_conn;
            write(g_payload.data(),g_payload.size());
        }
        else if (ev == JZ_NET_EV_CLOSE)
        {
        }
        else if (ev == JZ_NET_EV_READ)
        {
            //jzDebug() << "JZ_NET_EV_READ";
            write(g_payload.data(),g_payload.size());
            if(isClient)
            {
                g_sent++;
            }
        }
        else if (ev == JZ_NET_EV_WRITE)
        {
            // 裸 TCP 不维护 m_pendingBytes
        }
        else if (ev == JZ_NET_EV_ERROR)
        {
        }
    }

    void write(const char *buf, int len) 
    {
        // 模拟 JZNetObject::postReqEvent: new JZNetReqEvent(Net_Write) + jzApp->postEvent(net) + 唤醒
        JZNetReqEvent *req = new JZNetReqEvent();
        req->netCmd = JZNetReqEvent::Net_Write;
        req->conn = m_conn;
        req->buffer = JZByteArray(buf, len);
        jzApp->postEvent(g_net, req);
        g_net->wakeUpNet(); 
    }

    uint64_t m_conn = INVALID_NET_HANDLE;
    bool isClient = false;
};


class MySockServer : public JZObject
{
public:
    MySockServer()
    {
    }

    
    JZObject *onAccept(uint64_t conn) 
    {
        MySockClient *s = new MySockClient();
        s->m_conn = conn;
        s->moveToThread(this->thread());

        jzDebug() << "onAccept" << conn << s;
        return s;
    }
    
    uint64_t m_conn = INVALID_NET_HANDLE;
};

// mongoose 事件回调: 除 MG_EV_ACCEPT 需在 net 线程本地标记 fn_data 外,
// 其余所有事件一律 new ResEvent 跨线程投递给 AppObj 处理(模拟 JZNetObject 把
// MG_EV_READ/WRITE/CLOSE 等事件都抛到业务线程处理)
static void mg_thread_fn_real(mg_connection *c, int ev, void *ev_data)
{
    (void)ev_data;
    if (ev == MG_EV_ACCEPT)
    {
        auto obj = g_sock_server->onAccept(c->id);
        bindConn(c, obj);
        g_clientServerConn = c->id;
        return;
    }

    // 关闭/析构期间不再投递(避免 jzApp 已析构后 postEvent 崩溃)
    if (!g_posting.load())
        return;

    // 把事件整体转发给 AppObj(主线程)处理, 使用真实的 JZNetResEvent
    JZNetResEvent *res = new JZNetResEvent();
    res->event = (JZNetEvent)ev;
    res->conn = c->id;
    if (ev == MG_EV_READ && c->recv.len > 0)
    {
        res->binData = JZByteArray((const char *)c->recv.buf, c->recv.len);
        mg_iobuf_del(&c->recv, 0, c->recv.len);   // client 侧: 丢弃
    }

    auto it = g_newObjectMap.find(c->id);
    if(it != g_newObjectMap.end())
    {
        JZObject *obj = it->second;
        jzApp->postEvent(obj, res);   // 跨线程: net 线程 -> 主线程 app
    }
    else
    {
        if(!g_newObjectMap.empty())
        {
            jzDebug() << g_newObjectMap << c->id;
        }
    }
}

int main(int argc, char **argv)
{
    mg_log_level = 1;
    int runMs = 15000;
    int port = 9001;
    for (int i = 1; i < argc; i++)
    {
        std::string a = argv[i];
        if (a == "--time" && i + 1 < argc) runMs = atoi(argv[++i]);
        else if (a == "--port" && i + 1 < argc) port = atoi(argv[++i]);
        else if (a == "--poll" && i + 1 < argc) g_pollMs = atoi(argv[++i]);
        else if (a == "--heavy") g_heavy = true;   // 模拟 JZNet 的 m_objectMutex + m_connMap 开销
    }

    JZApplication app;   // 创建 jzApp, 主线程事件循环

    g_payload.assign(kPayloadLen, 'A');   // 1KB 负载, 与 test_jz_tcp_cpp 一致

    NetObj net;
    g_net = &net;

    net.clientConn = nullptr;
    
    std::string listenUrl = "tcp://127.0.0.1:" + std::to_string(port);
    mg_connection *srv = mg_listen(&net.m_mgr, listenUrl.c_str(), mg_thread_fn_real, nullptr);
    if (!srv) { printf("listen failed\n"); return 1; }
    mg_connection *cli = mg_connect(&net.m_mgr, listenUrl.c_str(), mg_thread_fn_real, nullptr);
    if (!cli) { printf("connect failed\n"); return 1; }
    net.clientConn = cli;
    
    MySockClient client;
    MySockServer server;

    g_sock_server = &server;
    server.m_conn = srv->id;
    client.m_conn = cli->id;

    client.isClient = true;
    bindConn(srv, &server);
    bindConn(cli, &client);

    g_clientConn = cli->id;

    net.start();   // 启动 net 线程(run() 循环 mg_mgr_poll + processEvents)

    int test_time = 0;
    JZTimer timer;
    connect(&timer.sigTimeout,[&test_time]{
        test_time++;

        double sec = test_time;
        long long cnt = g_sent.load();

        printf("[mg_postevent poll=%dms heavy=%d] TPS = %.0f  (sent=%lld, %.2fs)\n",
           g_pollMs, g_heavy ? 1 : 0, sec > 0 ? cnt / sec : 0.0, cnt, sec);

        if(test_time == 15)
            jzApp->quit();
    });
    timer.start(1000);

    app.exec();

    g_running.store(false);
    g_posting.store(false);   // 停止向已即将析构的 jzApp 投递事件
    net.quit();
    net.wait();
    mg_mgr_free(&net.m_mgr);  // 此时 jzApp/appObj 仍存活, 回调不再 postEvent, 安全

    double sec = runMs / 1000.0;
    long long cnt = g_sent.load();
    printf("[mg_postevent poll=%dms heavy=%d] TPS = %.0f  (sent=%lld, %.2fs)\n",
           g_pollMs, g_heavy ? 1 : 0, sec > 0 ? cnt / sec : 0.0, cnt, sec);
    return 0;
}
