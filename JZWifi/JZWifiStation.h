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

    // 仅允许 WiFi 驱动基类访问受保护的静态分发方法,
    // 具体驱动(如 JZWifiDriverEsp32)统一通过 JZWifiDriver 的转发中转调用。
    friend class JZWifiDriver;

public:
    JZWifiStation(JZObject *parent = nullptr);
    ~JZWifiStation() override;

    // ==================== Station (连接) ====================
    void connect(const JZWifiConfig &config);
    void disconnect();
    bool isConnected() const;
    std::string ipAddress() const;

jz_signals:
    JZSignal<> sigConnected;
    JZSignal<> sigDisconnected;
    JZSignal<int, const std::string&> sigError;

protected:
    static void onConnected(JZWifiStation *self, const char *ip);
    static void onDisconnected(JZWifiStation *self);
    static void onError(JZWifiStation *self, int code, const char *msg);

    JZWifiDriver *getDriver();

    bool m_connected = false;
    std::string m_ip;
};

#endif // !JZ_WIFI_STATION_H_
