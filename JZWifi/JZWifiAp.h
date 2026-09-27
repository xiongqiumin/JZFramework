#ifndef JZ_WIFI_AP_H_
#define JZ_WIFI_AP_H_
#include "JZObject.h"
#include "JZSignal.h"
#include "JZWifiDriver.h"

// JZWifiAp 表示 WiFi 接入点(AP / SoftAP)角色: 本机作为软 AP 对外提供服务 (做服务)。
// 底层操作委派给单例 JZWifiDriver。底层事件由驱动经 JZWifiDriver 的转发中转
// 回到本类的静态分发方法 (本类是 JZWifiDriver 的 friend), 再以信号形式通知上层,
// 与 JZBLEPeripheral 的设计一致。
class JZFRAMEWORK_API JZWifiAp : public JZObject
{
    JZ_OBJECT_DECLARE(JZWifiAp)

    // 仅允许 WiFi 驱动基类访问受保护的静态分发方法,
    // 具体驱动(如 JZWifiDriverEsp32)统一通过 JZWifiDriver 的转发中转调用。
    friend class JZWifiDriver;

public:
    enum State{
        None,
        Starting,
        Started,    
        Stoping,    
    };

    JZWifiAp(JZObject *parent = nullptr);
    ~JZWifiAp() override;

    // ==================== Access Point (做服务) ====================
    void start(const JZWifiApConfig &config);
    void stop();
    bool isRunning() const;
    int connectedStationCount() const;

    State state() const;

jz_signals:
    JZSignal<> sigStarted;
    JZSignal<> sigStopped;
    JZSignal<const std::string&, const std::string&> sigStationConnected;    // mac, ip
    JZSignal<const std::string&, const std::string&> sigStationDisconnected; // mac, ip

protected:
    static void onStarted(JZObjectPointer self);
    static void onStopped(JZObjectPointer self);
    static void onStationConnected(JZObjectPointer self, const char *mac, const char *ip);
    static void onStationDisconnected(JZObjectPointer self, const char *mac, const char *ip);

    State m_state = None;
    int m_stationCount = 0;
    JZWifiApConfig m_config;
};

#endif // !JZ_WIFI_AP_H_
