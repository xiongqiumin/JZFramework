#include "JZApplication.h"
#include "JZDebug.h"
#include "JZCommandLineParser.h"
#include "net_test_common.h"

// 各模块单独实现, 暴露独立的 test_xxx() 入口
int test_tcp();
int test_udp();
int test_websocket();
int test_mqtt();
int test_http();
int test_netServer();
int test_netCrypto();
int test_netAddr();

int main(int argc, char *argv[])
{
    JZApplication a;

    std::string mode = "local";
    std::string serverHost = "127.0.0.1";
    JZCommandLineParser parser;
    parser.addOption("--mode", &mode);
    parser.addOption("--server", &serverHost);
    parser.parse(argc, argv);
    g_option.host = serverHost;

    // 模式: local(回环, 默认) / server(仅服务器) / client(仅客户端)
    if (mode == "server")
    {
        g_option.enableServer = true;
        g_option.enableClient = false;
        g_option.loopTime = 30000;   // 等待客户端连接 30s
        g_option.delayTime = 0;
    }
    else if (mode == "client")
    {
        g_option.enableServer = false;
        g_option.enableClient = true;
        g_option.delayTime = 10 * 1000;
    }
    else if (mode == "local")
    {
        g_option.enableServer = true;
        g_option.enableClient = true;
        g_option.delayTime = 0;
    }
    else
    {
        jzDebug() << "[错误] 未知模式 '" << mode << "', 仅支持: local / server / client";
        return 1;
    }

    jzDebug() << "########## Net 模块综合测试 (mode=" << mode
        << "): 逐个运行 TCP / UDP / WebSocket / MQTT / HTTP / JZNetServer ##########";

    test_netAddr();
    jzDebug() << "【OK】连接地址(accept / connect) 通过";

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

    test_netCrypto();
    jzDebug() << "【OK】Net 加密测试通过";

    jzDebug() << "【Benchmark】PASS: 所有 net 模块均正常收发 (TCP/UDP/WebSocket/MQTT/HTTP/JZNetServer/加密)";

    return 0;
}
