#include <string>
#include <vector>

#include "JZDebug.h"
#include "JZApplication.h"
#include "JZEventLoop.h"
#include "JZByteArray.h"

// ===================== 协程网络库 =====================
#include "JZCo.h"
#include "JZCoTcpSocket.h"
#include "JZCoTcpServer.h"

// ===================== 测试公用工具 =====================
#include "co_test_common.h"    // CO_TEST_ASSERT / coReadLine / coMakeMsg / coParseMsg

using namespace std;

// ============ 协程 TCP: server echo 回环 (ping-pong) ============
static void testCoTcp()
{
    jzDebug() << "########## 协程 TCP 测试 (echo 回环) ##########";
    const int port = 18200;
    const int rounds = 50;

    auto serverCo = jzco_spawn([&]() {
        JZCoTcpServer server;
        CO_TEST_ASSERT(server.listen(port), "TCP server listen 失败");
        JZCoTcpSocket *cli = server.accept();
        CO_TEST_ASSERT(cli && cli->isConnected(), "server accept 失败");
        for (int i = 0; i < rounds; i++)
        {
            JZByteArray buf = coReadLine(cli);
            cli->write(buf);          // 原样回传
        }
        // 收尾: 等客户端把最后一个 echo 读完并主动断开(这里读到连接关闭)再关连接。
        // ⚠ 不能写完最后一个 echo 就 disconnectFromHost(): 客户端可能"已经收到、还没读走"这条数据,
        //    连接一关, readAll() 里 waitForReadyRead 之后是按 isConnected() 判成败的, 那份数据会被丢掉,
        //    末轮 coReadLine 偶发读到空 → 断言失败(Debug 下更容易撞上)。
        //    客户端只会发 rounds 条消息, 所以这个循环总是等到"对方断开"才退出。
        while (cli->readAll().isOk())
        {
        }
        cli->disconnectFromHost();
    });

    auto clientCo = jzco_spawn([&]() {
        JZCoTcpSocket client;
        CO_TEST_ASSERT(client.connectToHost("127.0.0.1", port), "client connect 失败");
        for (int i = 0; i < rounds; i++)
        {
            client.write(str_toUtf8(coMakeMsg("tcp", i)));
            JZByteArray buf = coReadLine(&client);
            uint64_t seq;
            CO_TEST_ASSERT(coParseMsg(str_fromUtf8(buf), "tcp", seq) && seq == (uint64_t)i,
                           "TCP echo 校验失败 期望=" + to_string(i) + " 收到=" + str_fromUtf8(buf));
        }
        client.disconnectFromHost();
    });

    jzco_waitAll({serverCo, clientCo});
    jzDebug() << "【OK】协程 TCP 回环通过 (" << rounds << " 轮 echo)";
}

// ===================== 测试入口 (供 test_co.cpp 编排调用) =====================
int test_coTcp()
{
    testCoTcp();
    return 0;
}
