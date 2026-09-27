#ifndef JZ_WIFI_DRIVER_SIMULATOR_H_
#define JZ_WIFI_DRIVER_SIMULATOR_H_
#include "JZWifiDriver.h"
#include "JZWifiStation.h"
#include "JZWifiAp.h"
#include "JZWifiScanner.h"
#include "JZSimPack.h"
#include "JZSimServer.h"
#include "JZObject.h"
#include "JZSignal.h"
#include "JZObjectPointer.h"
#include <map>
#include <string>

// WiFi 模拟驱动: 与 JZGpioDriverSimulator / JZBleDriverSimulator 类似,
// 通过 JZSimServer (内嵌 HTTP Server) 把 WiFi 操作上报; 网页/API 触发的动作
// 经 JZSimServer 路由回 onRecv。应用侧可像使用 Esp32 驱动一样使用它。
//
// 适用场景: 在 PC 上联调上层 WiFi 相关业务逻辑(Station / AP / Scan)而无需 ESP32 真机。
//
// 连接规则: Station 连接外部 AP 时, 只对 ssid="jz_simulator_ap" 且 password="123456"
// 视为成功(本地校验, 不发网络包), 其它 SSID/密码一律返回失败。扫描也仅返回本地固定结果。
// 只有 AP 的启动/停止需要发往模拟器(上行 ap_start/ap_stop),
// 站点(station)的接入与断开由模拟器通过下行 station_info 包回传,
// 驱动据此与本地集合做差分, 触发接入/断开事件。
class JZFRAMEWORK_API JZWifiDriverSimulator : public JZWifiDriver
{
    JZ_OBJECT_DECLARE(JZWifiDriverSimulator)
public:
    JZWifiDriverSimulator(const std::string &ip = "127.0.0.1", int port = 19123);
    ~JZWifiDriverSimulator() override;

    // 取当前 WiFi 驱动单例(仅当其为模拟驱动时返回, 否则 nullptr)
    static JZWifiDriverSimulator *simInstance();

    // 每次本驱动与模拟器交互(sendApCmd 发出指令 / onRecv 收到回发)都会通过此信号广播。
    JZSignal<JZSimCmd, const JZVariantMap&> sigSim{this};

    bool isWifiAvailable();

    // === Station (连接) ===
    void stationConnect(JZWifiStation *w, const JZWifiConfig &cfg) override;
    void stationDisconnect(JZWifiStation *w) override;
    void stationSwitch(JZWifiStation *w, const JZWifiConfig &cfg) override;
    bool isStationConnected(JZWifiStation *w) override;
    std::string stationIpAddress(JZWifiStation *w) override;
    int stationRssi(JZWifiStation *w) override;

    // === Access Point (做服务) ===
    void apStart(JZWifiAp *w, const JZWifiApConfig &cfg) override;
    void apStop(JZWifiAp *w) override;
    bool isApRunning(JZWifiAp *w) override;
    int  apStationCount(JZWifiAp *w) override;

    // === Scan (扫描) ===
    void scanStart(JZWifiScanner *s, const JZScanConfig &cfg) override;
    void scanStop(JZWifiScanner *s) override;
    bool isScanning(JZWifiScanner *s) override;

private:
    // 下行路由判断 / 下行处理(由注册进 JZSimServer 的 lambda 转发, 不再多继承)。
    bool support(JZSimCmd cmd) const;
    void onRecv(const JZSimPack &pack);

    void sendApCmd(const JZSimPack &pack); // 上行: Driver->模拟器 (ap_start/ap_stop)
    // 配置 wif.aps 变更后: 若当前 station 凭证在新配置中已无匹配 AP, 断开 station。
    void checkStationCredential();

    JZSimServer *m_sim = nullptr;
    int m_simToken = 0;               // JZSimServer 注册令牌
    JZObjectPointer m_apWifi;        // 当前 AP 对象(用于回传站点事件)
    JZObjectPointer m_staWifi;       // 当前 Station 对象(连接请求发出后即记录, 等待模拟器回发结果)
    std::string m_staSsid;           // 当前 station 连接所用的 ssid(用于判断配置变更是否使其失效)
    std::string m_staPassword;       // 当前 station 连接所用的 password
    std::map<std::string, std::string> m_apStations; // 当前连接到本 AP 的站点: mac -> ip
};

#endif // !JZ_WIFI_DRIVER_SIMULATOR_H_
