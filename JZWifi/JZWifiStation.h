#ifndef JZ_WIFI_STATION_H_
#define JZ_WIFI_STATION_H_

#include "JZObject.h"
#include "JZSignal.h"
#include "JZWifiDriver.h"

// JZWifiStation 表示 WiFi 站点(Station)角色: 作为客户端连接外部 AP (连接)。
// 底层操作委派给单例 JZWifiDriver。底层事件由驱动经 JZWifiDriver 的转发中转
// 回到本类的静态分发方法 (本类是 JZWifiDriver 的 friend), 再以信号形式通知上层,
// 与 JZBLEController 的设计一致。
class JZWifiStation : public JZObject
{
    JZ_OBJECT_DECLARE(JZWifiStation)    

public:
    enum State{
        None,
        Connecting,
        Connected,
        Disconnecting,
        Switching,       // 已连接态切换到另一 AP 的过程中(结果经 sigSwitched 上报, 不再走 sigDisconnected)
    };

    JZWifiStation(JZObject *parent = nullptr);
    ~JZWifiStation() override;

    // ==================== Station (连接) ====================
    void connectToAp(const JZWifiConfig &config);
    void disconnectFromAp();
    // 已连接态切换到另一个 AP(同 SSID 更强 BSSID 或不同 SSID), 不停回 None, 无额外延迟
    void switchToAp(const JZWifiConfig &config);
    bool isConnected() const;    
    State state() const;
    std::string ipAddress() const;

    // 当前连接 AP 的信号强度(dBm), 未连接返回 -127
    int currentRssi() const;

    // 当前连接 AP 的 BSSID(MAC 字符串), 未连接返回空串(由连接事件缓存, 非实时查询)
    std::string bssid() const;

    // 将 RSSI(dBm) 转换为 0~5 格信号强度, 统一阈值, 与平台无关
    //   5: >= -50  (极强)
    //   4: >= -60  (良好)
    //   3: >= -70  (一般)
    //   2: >= -80  (较弱)
    //   1: >= -90  (微弱)
    //   0: <  -90  (无信号/未连接)
    int signalStrength() const;

jz_signals:
    JZSignal<> sigConnected;
    JZSignal<> sigDisconnected;
    JZSignal<> sigSwitched;     // 切换结果(成功/失败), 结果经 state()/bssid() 查询; 与 sigDisconnected 区分: 切换失败不发包 sigDisconnected
    JZSignal<int, const std::string&> sigError;

protected:
    // 仅允许 WiFi 驱动基类访问受保护的静态分发方法,
    // 具体驱动(如 JZWifiDriverEsp32)统一通过 JZWifiDriver 的转发中转调用。
    friend class JZWifiDriver;

    static void onConnected(JZObjectPointer self, const char *ip, const char *bssid);
    static void onDisconnected(JZObjectPointer self);
    static void onError(JZObjectPointer self, int code, const char *msg);
    static void onStationSwitched(JZObjectPointer self, bool ok, const char *bssid);  // 切换结果(ok=false 表示失败), 更新状态并发 sigSwitched

    State m_state = None;
    std::string m_ip;
    std::string m_bssid;        // 当前连接 AP 的 BSSID(MAC), 由连接事件回传缓存(非实时查询)
};

#endif // !JZ_WIFI_STATION_H_
