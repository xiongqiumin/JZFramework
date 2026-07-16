#ifndef JZ_WIFI_DRIVER_SIMULATOR_H_
#define JZ_WIFI_DRIVER_SIMULATOR_H_

#include "JZWifiDriver.h"
#include "JZWifiStation.h"
#include "JZWifiAp.h"
#include "JZWifiScanner.h"
#include "JZSimPack.h"
#include "JZNetClient.h"
#include "JZObjectPointer.h"
#include <map>
#include <string>

// WiFi 模拟驱动: 与 JZGpioDriverSimulator / JZBleDriverSimulator 类似,
// 通过 JZNetClient 把 WiFi 操作封装成网络包发往 JZSimulatorDevice (运行 JZNetServer + LVGL 界面),
// 由后者模拟真实的 WiFi 芯片行为并把结果/事件回传。应用侧可像使用 Esp32 驱动一样使用它。
//
// 适用场景: 在 PC 上联调上层 WiFi 相关业务逻辑(Station / AP / Scan)而无需 ESP32 真机。
//
// 连接规则: Station 连接外部 AP 时, 只对 ssid="jz_simulator_ap" 且 password="123456"
// 视为成功(本地校验, 不发网络包), 其它 SSID/密码一律返回失败。扫描也仅返回本地固定结果。
// 只有 AP 的启动/停止需要发往模拟器(上行 ap_start/ap_stop),
// 站点(station)的接入与断开由模拟器通过下行 station_info 包回传,
// 驱动据此与本地集合做差分, 触发接入/断开事件。
class JZWifiDriverSimulator : public JZWifiDriver
{
public:
    JZWifiDriverSimulator(const std::string &ip = "127.0.0.1", int port = 19123);
    ~JZWifiDriverSimulator() override;

    bool isWifiAvailable() override;

    // === Station (连接) ===
    void stationConnect(JZWifiStation *w, const JZWifiConfig &cfg) override;
    void stationDisconnect(JZWifiStation *w) override;
    bool isStationConnected(JZWifiStation *w) override;
    std::string stationIpAddress(JZWifiStation *w) override;
    int stationRssi(JZWifiStation *w) override;

    // === Access Point (做服务) ===
    void apStart(JZWifiAp *w, const JZWifiApConfig &cfg) override;
    void apStop(JZWifiAp *w) override;
    bool isApRunning(JZWifiAp *w) override;
    int  apStationCount(JZWifiAp *w) override;

    // === Scan (扫描) ===
    void scanStart(JZWifiScanner *s) override;
    void scanStop(JZWifiScanner *s) override;
    bool isScanning(JZWifiScanner *s) override;

private:
    void onPacket(JZNetPackPtr pack);                 // 来自模拟器的事件
    void sendApCmd(const JZSimPack &pack); // 上行: Driver->模拟器 (ap_start/ap_stop)

    // Station 连接只接受这组凭证
    static constexpr const char *kApSsid = "jz_simulator_ap";
    static constexpr const char *kApPassword = "123456";

    JZNetClient   *m_client = nullptr;
    JZObjectPointer m_apWifi;        // 当前 AP 对象(用于回传站点事件)
    JZObjectPointer m_staWifi;       // 当前 Station 对象
    std::map<std::string, std::string> m_apStations; // 当前连接到本 AP 的站点: mac -> ip
};

#endif // !JZ_WIFI_DRIVER_SIMULATOR_H_
