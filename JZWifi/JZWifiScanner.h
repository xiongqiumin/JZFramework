#ifndef JZ_WIFI_SCANNER_H_
#define JZ_WIFI_SCANNER_H_

#include "JZObject.h"
#include "JZSignal.h"
#include "JZWifiDriver.h"

// JZWifiScanner 扫描周围 WiFi 接入点, 逐个返回 ssid 与信号强度(rssi)。
// 底层操作委派给单例 JZWifiDriver。扫描结果经 JZWifiDriver 的转发中转回到本类
// 的静态分发方法 (本类是 JZWifiDriver 的 friend), 再以信号形式通知上层,
// 与 JZBLEDeviceDiscoveryAgent 的设计一致。
class JZWifiScanner : public JZObject
{
    JZ_OBJECT_DECLARE(JZWifiScanner)

    // 仅允许 WiFi 驱动基类访问受保护的静态分发方法,
    // 具体驱动(如 JZWifiDriverEsp32)统一通过 JZWifiDriver 的转发中转调用。
    friend class JZWifiDriver;

public:
    JZWifiScanner(JZObject *parent = nullptr);
    ~JZWifiScanner() override;

    void start();
    void stop();
    bool isActive() const;

    // 最近一次扫描发现的 AP 列表 (按信号强度降序)
    std::vector<JZWifiScanResult> results() const;

jz_signals:    
    JZSignal<const JZWifiScanResult&> sigFinished; //扫描完成
    JZSignal<int, const std::string&> sigError;

protected:    
    static void onFinished(JZObjectPointer self, const JZWifiScanResult &r);
    static void onError(JZObjectPointer self, int code, const char *msg);

    JZWifiDriver *getDriver();

    bool m_active = false;
    std::vector<JZWifiScanResult> m_results;
};

#endif // !JZ_WIFI_SCANNER_H_
