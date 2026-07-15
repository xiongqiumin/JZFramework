#include <string>
#include <vector>

#include "JZDebug.h"
#include "JZApplication.h"
#include "JZByteArray.h"

// ===================== 协程网络库 =====================
#include "JZCo.h"
#include "JZCoUdpSocket.h"

// ===================== 测试公用工具 =====================
#include "co_test_common.h"    // CO_TEST_ASSERT / coMakeMsg / coParseMsg

using namespace std;

// ============ 协程 UDP: server echo 回环 (ping-pong, 按数据报边界) ============
static void testCoUdp()
{
    jzDebug() << "########## 协程 UDP 测试 (echo 回环) ##########";
    const int port = 18201;
    const int rounds = 50;

    auto serverCo = jzco_spawn([&]() {
        JZCoUdpSocket srv;
        CO_TEST_ASSERT(srv.bind(port), "UDP server bind 失败");
        for (int i = 0; i < rounds; i++)
        {
            auto d = srv.receive();
            CO_TEST_ASSERT(d.isOk(), "UDP server receive 失败: " + d.errorMessage());
            // 回显给发送方(数据报边界 + 发送方地址均保留)
            srv.writeDatagram(d.value().peerIp, d.value().peerPort, d.value().data);
        }
    });

    auto clientCo = jzco_spawn([&]() {
        JZCoUdpSocket cli;
        CO_TEST_ASSERT(cli.bind(0), "UDP client bind 失败");
        for (int i = 0; i < rounds; i++)
        {
            cli.writeDatagram("127.0.0.1", port, str_toUtf8(coMakeMsg("udp", i)));
            auto d = cli.receive();
            CO_TEST_ASSERT(d.isOk(), "UDP client receive 失败: " + d.errorMessage());
            uint64_t seq;
            CO_TEST_ASSERT(coParseMsg(str_fromUtf8(d.value().data), "udp", seq) && seq == (uint64_t)i,
                           "UDP echo 校验失败 期望=" + to_string(i) + " 收到=" + str_fromUtf8(d.value().data));
        }
    });

    jzco_waitAll({serverCo, clientCo});
    jzDebug() << "【OK】协程 UDP 回环通过 (" << rounds << " 轮 echo)";
}

// ===================== 测试入口 (供 test_co.cpp 编排调用) =====================
int test_coUdp()
{
    testCoUdp();
    return 0;
}
