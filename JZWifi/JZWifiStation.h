#ifndef JZ_WIFI_STATION_H_
#define JZ_WIFI_STATION_H_

#include "JZObject.h"
#include "JZSignal.h"
#include "JZWifiDriver.h"

// JZWifiStation 表示 WiFi 站点(Station)角色: 作为客户端连接外部 AP (连接)。
// 底层操作委派给单例 JZWifiDriver。底层事件由驱动经 JZWifiDriver 的转发中转
// 回到本类的静态分发方法 (本类是 JZWifiDriver 的 friend), 再以信号形式通知上层,
// 与 JZBLECentral 的设计一致。
class JZWifiStation : public JZObject
{
    JZ_OBJECT_DECLARE(JZWifiStation)    

public:
    enum State{
        None,
        Connecting,
        Connected,
        Disconnecting,
    };

    JZWifiStation(JZObject *parent = nullptr);
    ~JZWifiStation() override;

    // ==================== Station (连接) ====================
    void connectToAp(const JZWifiConfig &config);
    void disconnectFromAp();
    bool isConnected() const;    
    State state() const;
    std::string ipAddress() const;

    // 当前连接 AP 的信号强度(dBm), 未连接返回 -127
    int rssi() const;

    // 将 RSSI(dBm) 转换为 0~5 格信号强度, 统一阈值, 与平台无关
    //   5: >= -50  (极强)
    //   4: >= -60  (良好)
    //   3: >= -70  (一般)
    //   2: >= -80  (较弱)
    //   1: >= -90  (微弱)
    //   0: <  -90  (无信号/未连接)
    int signalStrength() const;

    // 定时轮询 RSSI 并在变化时发射 sigRssiChanged, intervalMs 为轮询间隔
    void startRssiMonitor(int intervalMs = 5000);
    void stopRssiMonitor();

jz_signals:
    JZSignal<> sigConnected;
    JZSignal<> sigDisconnected;
    JZSignal<int, const std::string&> sigError;
    JZSignal<int> sigRssiChanged;       // RSSI 变化时发射, 参数为当前 dBm 值

protected:
    // 仅允许 WiFi 驱动基类访问受保护的静态分发方法,
    // 具体驱动(如 JZWifiDriverEsp32)统一通过 JZWifiDriver 的转发中转调用。
    friend class JZWifiDriver;

    static void onConnected(JZObjectPointer self, const char *ip);
    static void onDisconnected(JZObjectPointer self);
    static void onError(JZObjectPointer self, int code, const char *msg);

    void timerEvent(JZTimerEvent *event) override;

    State m_state = None;
    std::string m_ip;
    int m_rssiTimerId = INVAILD_TIMER_ID;
    int m_lastRssi = -127;
};

#endif // !JZ_WIFI_STATION_H_
