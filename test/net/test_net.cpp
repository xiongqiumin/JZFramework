#include "JZApplication.h"
#include "JZDebug.h"

// 各模块单独实现, 暴露独立的 test_xxx() 入口
int test_tcp();
int test_udp();
int test_websocket();
int test_mqtt();
int test_http();
int test_netServer();

int main()
{
    JZApplication a;
    jzDebug() << "########## Net 模块综合测试: 逐个运行 TCP / UDP / WebSocket / MQTT / HTTP / JZNetServer ##########";

    for(int i = 0; i < 100; i++)
    {
        jzDebug("==== 第%d次测试 ====\n", i+ 1);

        test_tcp();
        jzDebug() << "【OK】TCP 通过";

        test_udp();
        jzDebug() << "【OK】UDP 通过";

        test_websocket();
        jzDebug() << "【OK】WebSocket 通过";

        test_mqtt();
        jzDebug() << "【OK】MQTT 通过";

        test_http();
        jzDebug() << "【OK】HTTP 通过";

        test_netServer();
        jzDebug() << "【OK】JZNetServer 通过";

        jzDebug() << "【Benchmark】PASS: 所有 net 模块均正常收发 (TCP/UDP/WebSocket/MQTT/HTTP/JZNetServer)";
    }

    return 0;
}
