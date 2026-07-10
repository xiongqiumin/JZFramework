#include <string>
#include <vector>
#include "JZWifi.h"
#include "JZWifiDriver.h"
#include "JZDebug.h"
#include "JZApplication.h"
#include "JZCommandLineParser.h"

using namespace std;

// JZWifi 扫描 + 自动连接测试
// 用法:
//   test_wifiAuto <ssid1> <pwd1> [<ssid2> <pwd2> ...]
//   按 ssid/password 交替给出候选网络序列; 不提供则仅演示 API
int main(int argc, char *argv[])
{
    JZApplication app;

    vector<JZWifiCredential> cands;
    for (int i = 1; i + 1 < argc; i += 2) {
        JZWifiCredential c;
        c.ssid = argv[i];
        c.password = argv[i + 1];
        cands.push_back(c);
    }
    if (cands.empty())
        jzDebug() << "用法: test_wifiAuto <ssid1> <pwd1> [<ssid2> <pwd2> ...]";

    if (!JZWifiDriver::instance())
        jzDebug() << "未设置 JZWifiDriver 实例 (当前平台无可用驱动), 仅演示 API";

    // === 自动连接服务 ===
    JZWifiAutoConnectService svc;
    svc.setCandidates(cands);
    svc.setScanIntervalMs(10000);
    connect(&svc.sigConnecting, [](const string &ssid) {
        jzDebug() << "自动连接: 正在连接" << ssid;
    });
    connect(&svc.sigConnected, [](const string &ssid) {
        jzDebug() << "自动连接: 已连接" << ssid;
    });
    connect(&svc.sigError, [](int code, const string &msg) {
        jzDebug() << "自动连接: 错误(" << code << "):" << msg;
    });
    svc.start();

    // === 单独演示扫描器 ===
    JZWifiScanner scanner;
    connect(&scanner.sigResult, [](const JZWifiScanResult &r) {
        jzDebug() << "扫描到:" << r.ssid
                  << " rssi=" << r.rssi
                  << " channel=" << r.channel;
    });
    connect(&scanner.sigFinished, []() {
        jzDebug() << "扫描完成";
    });
    scanner.start();

    return app.exec();
}
