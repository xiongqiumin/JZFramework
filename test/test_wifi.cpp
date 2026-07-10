#include <string>
#include "JZWifi.h"
#include "JZWifiDriver.h"
#include "JZDebug.h"
#include "JZApplication.h"
#include "JZCommandLineParser.h"

using namespace std;

// JZWifi 测试 (拆分为 JZWifiStation 连接 / JZWifiAp 做服务)
// 用法:
//   test_wifi <ssid> <password> <apssid> <appassword>
//   参数均为可选: 提供 ssid 即尝试连接外部 AP; 提供 apssid 即本机作为 AP 提供服务
int main(int argc, char *argv[])
{
    JZApplication app;

    string ssid, password, apSsid, apPassword;
    JZCommandLineParser parser;
    parser.setDescription("JZWifi 测试 (JZWifiStation 连接 + JZWifiAp 做服务)");
    parser.addArgument("ssid", &ssid, false);
    parser.addArgument("password", &password, false);
    parser.addArgument("apssid", &apSsid, false);
    parser.addArgument("appassword", &apPassword, false);
    if (!parser.parse(argc, argv)) {
        parser.printHelp();
        return -1;
    }

    JZWifiStation station;
    JZWifiAp ap;
    if (!JZWifiDriver::instance())
        jzDebug() << "未设置 JZWifiDriver 实例 (当前平台无可用驱动), 仅演示 API";

    connect(&station.sigConnected, [&station]() {
        jzDebug() << "Station 已连接, IP:" << station.ipAddress();
    });
    connect(&station.sigDisconnected, []() {
        jzDebug() << "Station 已断开";
    });
    connect(&station.sigError, [](int code, const string &msg) {
        jzDebug() << "Station 错误(" << code << "):" << msg;
    });
    connect(&ap.sigStarted, [&ap]() {
        jzDebug() << "AP 已启动, IP:" << ap.ipAddress()
                  << " 当前连接数:" << ap.connectedStationCount();
    });
    connect(&ap.sigStopped, []() {
        jzDebug() << "AP 已停止";
    });
    connect(&ap.sigStationConnected, [](const string &mac, const string &ip) {
        jzDebug() << "AP 客户端连接:" << mac << (ip.empty() ? "" : (" IP:" + ip));
    });
    connect(&ap.sigStationDisconnected, [](const string &mac, const string &) {
        jzDebug() << "AP 客户端断开:" << mac;
    });

    if (!ssid.empty()) {
        JZWifiConfig cfg;
        cfg.ssid = ssid;
        cfg.password = password;
        jzDebug() << "连接 WiFi:" << ssid;
        station.connect(cfg);
    }

    if (!apSsid.empty()) {
        JZWifiApConfig apcfg;
        apcfg.ssid = apSsid;
        apcfg.password = apPassword;
        jzDebug() << "启动 AP:" << apSsid;
        ap.start(apcfg);
    }

    if (ssid.empty() && apSsid.empty())
        jzDebug() << "未提供任何参数, 用法: test_wifi <ssid> <password> <apssid> <appassword>";

    return app.exec();
}
