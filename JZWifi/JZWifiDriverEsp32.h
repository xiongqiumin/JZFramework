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
    bool m_apUp = false;
    bool m_inited = false;
    bool m_started = false;    
    std::string m_staIp;
    std::string m_apIp;
};

#endif // !JZ_WIFI_DRIVER_ESP32_H_
