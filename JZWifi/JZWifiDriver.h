#ifndef JZ_WIFI_DRIVER_H_
#define JZ_WIFI_DRIVER_H_
#include <string>
#include <vector>
#include "JZStd.h"
#include "JZObjectPointer.h"
#include "JZObject.h"

class JZWifiStation;
class JZWifiAp;
class JZWifiScanner;

// 站点(Station)连接配置: 连接到外部 AP
struct JZFRAMEWORK_API JZWifiConfig
{
    std::string ssid;
    std::string password;
    std::string bssid;            // 目标 AP 的 MAC (可选); 非空时连接时钉选该 BSSID, 空则交给驱动自选(用于同 SSID 漫游)
    bool dhcp = true;             // true: 动态获取 IP; false: 使用静态 IP
    std::string ip;               // 静态 IP (dhcp=false 时有效)
    std::string gateway;
    std::string subnet;
};

// 接入点(AP / SoftAP)配置: 本机作为服务对外提供 WiFi
struct JZFRAMEWORK_API JZWifiApConfig
{
    std::string ssid;
    std::string password;         // 空字符串表示开放网络
    int channel = 1;              // 1~13
    int maxConnections = 4;       // 1~10
    bool hidden = false;
    std::string ip = "192.168.4.1";
    std::string gateway = "192.168.4.1";
    std::string subnet = "255.255.255.0";
};

// WiFi 扫描配置: 描述一次扫描的行为(普通扫描 / 低影响漫游扫描)
struct JZFRAMEWORK_API JZScanConfig
{
    bool roamScan = false;        // 低影响漫游扫描: 底层驱动改用 passive + 可选仅扫当前信道, 减小对已建链路的打扰
    std::string ssid;             // 扫描过滤的 SSID(空=不过滤)
    bool scanAllChannels = true;  // true=扫全信道; false=仅扫当前信道(最低打扰, 但无法跨信道漫游)
};

// WiFi 扫描结果: 一个可被发现的 AP
struct JZFRAMEWORK_API JZWifiScanResult
{
    struct ApInfo
    {
        std::string ssid;          // 网络名
        int rssi = 0;              // 信号强度(dBm), 负值, 绝对值越小信号越强
        std::string bssid;         // AP MAC 地址
        int channel = 0;           // 主信道
        int authMode = 0;          // 加密方式 (平台相关, 如 ESP-IDF 的 WIFI_AUTH_*)
    };

    std::vector<ApInfo> apList;    
};

// WiFi 驱动抽象基类 (单例)。
// 具体平台(如 JZWifiDriverEsp32)实现其纯虚接口, 并通过本类的静态转发中转
// 把底层事件回传给 JZWifiStation / JZWifiAp / JZWifiScanner
// (三者均是 JZWifiDriver 的 friend, 受保护的静态分发方法不对外暴露)。
class JZFRAMEWORK_API JZWifiDriver : public JZObject
{
public:
    JZWifiDriver() = default;
    virtual ~JZWifiDriver() = default;

    static JZWifiDriver *instance();
    static void setInstance(JZWifiDriver *inst);

    // === Station (连接) ===
    // 调用方把自身(JZWifiStation*)作为 arg 透传, 事件回传时据此分发到对应对象
    virtual void stationConnect(JZWifiStation *w, const JZWifiConfig &cfg) = 0;
    virtual void stationDisconnect(JZWifiStation *w) = 0;
    virtual void stationSwitch(JZWifiStation *w, const JZWifiConfig &cfg) = 0;  // 已连接态切换到另一 AP(同/不同 SSID), 不停回 None
    virtual bool isStationConnected(JZWifiStation *w) = 0;
    virtual std::string stationIpAddress(JZWifiStation *w) = 0;
    virtual int stationRssi(JZWifiStation *w) = 0;

    // === Access Point (做服务) ===
    virtual void apStart(JZWifiAp *w, const JZWifiApConfig &cfg) = 0;
    virtual void apStop(JZWifiAp *w) = 0;
    virtual bool isApRunning(JZWifiAp *w) = 0;
    virtual int  apStationCount(JZWifiAp *w) = 0;

    // === Scan (扫描) ===
    virtual void scanStart(JZWifiScanner *s, const JZScanConfig &cfg) = 0;
    virtual void scanStop(JZWifiScanner *s) = 0;
    virtual bool isScanning(JZWifiScanner *s) = 0;

protected:
    JZ_DISABLE_COPY(JZWifiDriver)

    // === 事件转发中转 (JZWifiStation / JZWifiAp / JZWifiScanner 的 friend) ===
    static void onStationConnected(JZObjectPointer self, const char *ip, const char *bssid = "");
    static void onStationDisconnected(JZObjectPointer self);
    static void onStationError(JZObjectPointer self, int code, const char *msg);
    static void onStationSwitched(JZObjectPointer self, bool ok, const char *bssid = "");  // 切换结果: ok=false 表示 stationSwitch 失败
    static void onApStarted(JZObjectPointer self);
    static void onApStopped(JZObjectPointer self);
    static void onApStationConnected(JZObjectPointer self, const char *mac, const char *ip);
    static void onApStationDisconnected(JZObjectPointer self, const char *mac, const char *ip);    
    static void onScanFinished(JZObjectPointer self, const JZWifiScanResult &r);
    static void onScanError(JZObjectPointer self, int code, const char *msg);

    static JZWifiDriver *s_instance;
};

#endif // !JZ_WIFI_DRIVER_H_
