#include <atomic>
#include <chrono>
#include <string>
#include <vector>

#include "JZCo.h"
#include "JZCoWebSocket.h"
#include "JZCoWebSocketServer.h"
#include "JZNetworkRequest.h"
#include "JZTimer.h"
#include "JZDebug.h"
#include "JZApplication.h"

#include "co_test_common.h"    // CO_TEST_ASSERT (失败即 exit(1))

using namespace std;

class JZWebSocketTest : public JZObject
{
public:
    atomic<int> msgCount{0};
    chrono::steady_clock::time_point startTime;

    // 8客户端并发
    static const int CLIENT_NUM = 8;
    // 每客户端回环轮数(有限, 便于自动化测试结束)
    static const int ROUNDS = 50;

    vector<JZCoCoroutinePtr> client_cos;

    // 服务端
    JZCoWebSocketServer* co_server = nullptr;
    int port = 18000;

    // 测试停止标志(所有客户端完成回环后置位, 让服务端 accept 退出)
    atomic<bool> m_stop{false};
    atomic<int> m_finishedClients{0};

public:
    JZWebSocketTest() {
        // 启动TPS统计
        startTPSCount();

        // 启动服务端协程
        jzco_spawn([this]() {
            serverCoroutine();
        });

        // 启动8个客户端协程
        for (int i = 0; i < CLIENT_NUM; i++) {
            auto co = jzco_spawn([this, i]() {
                clientCoroutine(i);
            });
            client_cos.push_back(co);
        }
    }

    ~JZWebSocketTest() {
        jzco_waitAllCoroutines();
    }

    // ===================== TPS 统计 =====================
    void startTPSCount() {
        startTime = chrono::steady_clock::now();
        JZTimer* tpsTimer = new JZTimer(this);
        connect(&tpsTimer->sigTimeout, this, &JZWebSocketTest::printTPS);
        tpsTimer->start(1000);
    }

    void printTPS() {
        auto now = chrono::steady_clock::now();
        double elapsed = chrono::duration<double>(now - startTime).count();
        int count = msgCount.exchange(0);
        double tps = count / elapsed;
        jzDebug() << "【协程8并发】当前 TPS：" << tps;
        startTime = now;
    }

    // ===================== 服务端协程 =====================
    void serverCoroutine() {
        co_server = new JZCoWebSocketServer(this);
        if (!co_server->listen(port)) {
            jzDebug() << "服务端监听失败！";
            exit(-1);
        }

        jzDebug() << "服务端启动成功 port:" << port;

        // 协程循环 accept (置位 m_stop 后退出)
        while (!m_stop) {
            JZCoWebSocket* socket = co_server->accept(); // 协程等待
            if (!socket) continue;

            // 每个连接启动独立回显协程
            jzco_spawn([this, socket]() {
                while (true) {
                    string text;
                    // 协程等待消息
                    bool ok = socket->waitTextMessage(text);
                    if (!ok) break;

                    // 回显
                    socket->sendTextMessage(text);
                }
                socket->disconnectFromHost();
                delete socket;
            });
        }
    }

    // ===================== 客户端协程（单客户端逻辑） =====================
    void clientCoroutine(int clientId) {
        JZCoWebSocket client;
        string url = "ws://127.0.0.1:" + to_string(port);
        JZNetworkRequest req(url);

        uint64_t send_seq = 0;
        uint64_t expect_seq = 0;
        const string prefix = "hello world ";

        // 协程连接
        if (!client.connectToHost(req, 3000)) {
            jzDebug() << "客户端" << clientId << "连接失败";
            exit(-1);
        }

        jzDebug() << "客户端" << clientId << "连接成功";

        // 回环循环（协程同步写法, 有限轮数）
        while (send_seq < (uint64_t)ROUNDS) {
            // 1. 发送
            string send_msg = prefix + to_string(send_seq);
            client.sendTextMessage(send_msg);

            // 2. 协程等待接收
            string recv_msg;
            bool ok = client.waitTextMessage(recv_msg);
            if (!ok) {
                jzDebug() << "客户端" << clientId << "断开连接";
                exit(-1);
            }

            // ================= 校验 =================
            // 格式校验
            if (recv_msg.substr(0, prefix.size()) != prefix) {
                jzDebug() << "客户端" << clientId << "格式错误";
                exit(-1);
            }

            // 序号校验
            uint64_t recv_seq = stoull(recv_msg.substr(prefix.size()));
            if (recv_seq != expect_seq) {
                jzDebug() << "客户端" << clientId << "校验失败 期望:" << expect_seq << " 实际:" << recv_seq;
                exit(-1);
            }

            // 3. 序号递增
            send_seq++;
            expect_seq++;
            msgCount++;
        }

        // 客户端完成: 统计并决定是否让服务端停止 accept
        int n = ++m_finishedClients;
        if (n == CLIENT_NUM)
            m_stop = true;
    }
};

// ===================== 测试入口 (供 test_co.cpp 编排调用) =====================
int test_coWebSocket()
{
    jzDebug() << "########## 协程 WebSocket 测试 (8 客户端并发 echo 回环) ##########";

    JZWebSocketTest test;
    // 等 8 个客户端完成回环
    jzco_waitAll(test.client_cos);
    // 让服务端 accept 退出并等待所有协程结束
    test.m_stop = true;
    if (test.co_server)
        test.co_server->close();
    jzco_waitAllCoroutines();

    CO_TEST_ASSERT(test.m_finishedClients == JZWebSocketTest::CLIENT_NUM,
                   "部分客户端未跑完回环, 完成数=" + to_string(test.m_finishedClients));
    CO_TEST_ASSERT(test.msgCount == JZWebSocketTest::CLIENT_NUM * JZWebSocketTest::ROUNDS,
                   "回环消息总数不足, 实际=" + to_string(test.msgCount));

    jzDebug() << "【OK】协程 WebSocket 回环通过 (" << test.msgCount << " 条 echo)";
    return 0;
}
