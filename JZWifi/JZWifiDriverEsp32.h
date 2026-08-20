#ifndef JZ_WIFI_DRIVER_ESP32_H_
#define JZ_WIFI_DRIVER_ESP32_H_

#include "JZWifiDriver.h"
#include "JZObjectPointer.h"
#include <string>

// ESP32 (ESP-IDF) 平台下的 WiFi 驱动实现。
// 基于 esp_wifi + esp_netif + esp_event, 支持 Station / AP / APSTA 三种模式,
// 通过事件回调把底层结果经 JZWifiDriver 转发中转回 JZWifiStation / JZWifiAp / JZWifiScanner。
class JZWifiDriverEsp32 : public JZWifiDriver
{
public:
    JZWifiDriverEsp32();
    ~JZWifiDriverEsp32() override;

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

    // 事件回调入口 (由自由函数 wifi_event_handler 调用)
    void handleEvent(const char *event_base, int32_t event_id, void *event_data);

private:
    void ensureInit();
    void ensureStarted();
    void updateMode();

    JZObjectPointer m_scanTarget;       
    JZObjectPointer m_staWifi;
    JZObjectPointer m_apWifi;         
        
    bool m_staUp = false;       // 已连上并获取到 IP
    bool m_switching = false;   // stationSwitch 进行中(来自上层 switchToAp), 直到切换成功/失败才复位
    bool m_switchInterm = false;// 切换中间断开(esp_wifi_disconnect 触发)待吞掉标记
    bool m_apUp = false;
    bool m_inited = false;
    bool m_started = false;     // esp_wifi_start() 已返回 OK(WiFi 处于 STARTING/STARTED)
    bool m_staStarted = false;  // 已收到 WIFI_EVENT_STA_START, STA 真正就绪可扫描/连接
    std::string m_staIp;
    std::string m_apIp;
    std::string m_scanSsid;     // 扫描过滤的 SSID(透传到 wifi_scan_config_t.ssid, 需跨 scanStart 保持有效)
};

#endif // !JZ_WIFI_DRIVER_ESP32_H_
