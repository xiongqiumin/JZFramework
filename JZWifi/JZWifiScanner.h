#ifndef JZ_WIFI_SCANNER_H_
#define JZ_WIFI_SCANNER_H_
#include "JZObject.h"
#include "JZSignal.h"
#include "JZWifiDriver.h"

// JZWifiScanner 扫描周围 WiFi 接入点, 扫描完成后一次性返回所有结果 (ssid/信号强度等)。
// 底层操作委派给单例 JZWifiDriver。扫描结果经 JZWifiDriver 的转发中转回到本类
// 的静态分发方法 (本类是 JZWifiDriver 的 friend), 再以信号形式通知上层,
// 与 JZBLEDeviceDiscoveryAgent 的设计一致。
class JZFRAMEWORK_API JZWifiScanner : public JZObject
{
    JZ_OBJECT_DECLARE(JZWifiScanner)

    // 仅允许 WiFi 驱动基类访问受保护的静态分发方法,
    // 具体驱动(如 JZWifiDriverEsp32)统一通过 JZWifiDriver 的转发中转调用。
    friend class JZWifiDriver;

public:
    JZWifiScanner(JZObject *parent = nullptr);
    ~JZWifiScanner() override;

    void start();                                          // 普通扫描(全信道, 不过滤 SSID)
    void startRoamScan(const std::string &ssid, bool scanAllChannels); // 低影响漫游扫描: ssid=聚焦的 SSID, scanAllChannels=false 时仅扫当前信道
    void stop();
    bool isActive() const;
    const JZWifiScanResult &result() const;

jz_signals:
    JZSignal<const JZWifiScanResult&> sigFinished; //扫描完成
    JZSignal<int, const std::string&> sigError;

protected:
    void doStart();

    static void onFinished(JZObjectPointer self, const JZWifiScanResult &r);
    static void onError(JZObjectPointer self, int code, const char *msg);

    JZWifiScanResult m_result;

    bool m_active = false;
    JZScanConfig m_cfg;            // 本次扫描配置(普通 / 低影响漫游)
};

#endif // !JZ_WIFI_SCANNER_H_
