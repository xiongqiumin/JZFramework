#ifndef JZ_SIM_PACK_H_
#define JZ_SIM_PACK_H_

#include "JZNetPacket.h"
#include "JZVariant.h"
#include "JZResult.h"
#include <string>
#include <vector>
#include <json/value.h>

// ============================================================================
// 统一的模拟器命令枚举: 覆盖 BLE / GPIO / WiFi 等所有模拟器。
// 旧代码中分散在 JZBleSimPack / JZGpioSimPack / JZWifiSimPack 的命令常量名保持不变,
// 仅在此统一编号以避免冲突。所有模拟器共用同一个数据包 JZSimPack, 由 cmd 区分用途。
// ============================================================================
enum JZSimCmd {
    SIM_CMD_NONE = 0,

    // ---- BLE: Driver -> Device ----
    SIM_CMD_SCAN_START = 1,
    SIM_CMD_SCAN_STOP,
    SIM_CMD_CONNECT,
    SIM_CMD_DISCONNECT,
    SIM_CMD_DISCOVER_SVC,
    SIM_CMD_DISCOVER_CHR,
    SIM_CMD_READ,
    SIM_CMD_WRITE,
    SIM_CMD_ENABLE_NOTIFY,
    SIM_CMD_ADD_PERIPH_CHR,
    SIM_CMD_START_ADV,
    SIM_CMD_STOP_ADV,
    SIM_CMD_SEND_NOTIFY,
    SIM_CMD_SEND_INDICATE,

    // ---- BLE: Device -> Driver ----
    SIM_EVT_SCAN_RESULT,
    SIM_EVT_CONNECTED,
    SIM_EVT_DISCONNECTED,
    SIM_EVT_SERVICES,
    SIM_EVT_CHARACTERISTICS,
    SIM_EVT_READ_RESULT,
    SIM_EVT_WRITE_RESULT,
    SIM_EVT_NOTIFY_RX,
    SIM_EVT_CHAR_WRITTEN,
    SIM_EVT_ADV_STATE,
    SIM_EVT_LOG,
    SIM_EVT_BUSY,        // 服务器已达最大连接数 (单客户端限制), 拒绝新连接

    // ---- GPIO: Driver -> Device (编号从 100 起, 与 BLE 不冲突) ----
    SIM_GPIO_CMD_OPEN = 100,
    SIM_GPIO_CMD_CLOSE,
    SIM_GPIO_CMD_SET_DIR,
    SIM_GPIO_CMD_SET_PULL,
    SIM_GPIO_CMD_WRITE,
    SIM_GPIO_CMD_READ,
    SIM_GPIO_CMD_PWM_START,
    SIM_GPIO_CMD_PWM_FREQ,
    SIM_GPIO_CMD_PWM_DUTY,
    SIM_GPIO_CMD_PWM_STOP,
    SIM_GPIO_CMD_WAVE_START,
    SIM_GPIO_CMD_WAVE_STOP,
    SIM_GPIO_CMD_SET_TRIGGER,
    SIM_GPIO_EVT_LEVEL,
    SIM_GPIO_EVT_READ_RESULT,

    // ---- WiFi: Driver -> Device / Device -> Driver (编号从 200 起) ----
    SIM_WIFI_AP_START = 200,
    SIM_WIFI_AP_STOP,
    SIM_WIFI_STATION_INFO,
    // 配置细化事件(Device -> Driver): simServer 对 wifi.aps 做 diff 后发出,
    // 驱动在 onRecv 中按具体变更处理(如刷新 scan 结果 / 断开失效 station)。
    // 每个事件携带单个 ap 的完整对象(json 字段), REMOVED 携带被删 ap 的标识。
    SIM_WIFI_AP_ADDED   = 291,   // 新增一个外围 AP
    SIM_WIFI_AP_REMOVED = 292,   // 删除一个外围 AP
    SIM_WIFI_AP_UPDATED = 293,   // 某个外围 AP 字段变化(ssid/password/rssi/bssid/auth...)

    // ---- OTA: Driver -> Device / Device -> Driver (编号从 300 起) ----
    SIM_OTA_CMD_BASE = 300,

    // ---- Sensor: Driver -> Device / Device -> Driver (编号从 400 起) ----
    SIM_SENSOR_CMD_BASE = 400,
};

// 一个已接入本 AP 的站点(station)完整信息。
struct JZWifiStationInfo {
    std::string mac;   // 站点 MAC
    std::string ip;    // 站点分配到的 IP
};

// ============================================================================
// 唯一的模拟器数据包: 所有模拟器(BLE / GPIO / WiFi)共用此包。
//   - cmd : 命令类型(JZSimCmd 枚举)
//   - data: 命令参数 / 回传数据的扁平或嵌套 JZVariantMap
// 读写辅助函数已作为成员提供, 统一返回 JZResult<xxx>, 并支持多级 key(如 "a.b.c")。
// ============================================================================
class JZSimPack : public JZNetPack
{
public:
    static constexpr int PackType = NetPack_user + 1;

    JZSimPack();
    int packetType() const override;
    void saveToStream(JZDataStream &s) const override;
    void loadFromStream(JZDataStream &s) override;

    // 确保包类型已注册 (幂等)
    static void ensureRegistered();

    JZSimCmd     cmd = SIM_CMD_NONE;
    JZVariantMap data;
    // 可选的整体 JSON 载荷: 配置细化事件(如 SIM_WIFI_AP_*)用它承载单个 ap 对象。
    // 该事件不经网络序列化(进程内 onClientPacket 直投), 故无需进 save/loadFromStream。
    Json::Value   json;

    // ===== 写入 (key 支持多级, 如 "a.b.c", 中间节点自动建为 map) =====
    void setInt(const std::string &key, int v);
    void setInt64(const std::string &key, int64_t v);
    void setBool(const std::string &key, bool v);
    void setStr(const std::string &key, const std::string &v);
    void setStrList(const std::string &key, const std::vector<std::string> &v);
    void setIntList(const std::string &key, const std::vector<int> &v);
    void setDouble(const std::string &key, double v);
    void setDoubleList(const std::string &key, const std::vector<double> &v);
    void setMap(const std::string &key, const JZVariantMap &v);     // 嵌套 map
    void setList(const std::string &key, const JZVariantList &v);   // 嵌套 list

    // ===== 读取 (统一返回 JZResult, 支持多级 key) =====
    JZResult<int>                     getInt(const std::string &key) const;
    JZResult<int64_t>                 getInt64(const std::string &key) const;
    JZResult<bool>                    getBool(const std::string &key) const;
    JZResult<std::string>             getStr(const std::string &key) const;
    JZResult<std::vector<std::string>> getStrList(const std::string &key) const;
    JZResult<std::vector<int>>        getIntList(const std::string &key) const;
    JZResult<double>                  getDouble(const std::string &key) const;
    JZResult<std::vector<double>>     getDoubleList(const std::string &key) const;
    JZResult<JZVariantMap>            getMap(const std::string &key) const;  // 嵌套 map
    JZResult<JZVariantList>           getList(const std::string &key) const; // 嵌套 list

private:
    // 多级 key('.' 分隔)与嵌套序列化, 仅供成员函数内部使用
    static std::vector<std::string> splitKey(const std::string &key);
    static void setField(JZVariantMap &root, const std::string &key, const JZVariant &val);
    static JZResult<JZVariant> getField(const JZVariantMap &root, const std::string &key);
    static void writeVariant(JZDataStream &s, const JZVariant &v);
    static JZVariant readVariant(JZDataStream &s);
};

#endif // !JZ_SIM_PACK_H_
