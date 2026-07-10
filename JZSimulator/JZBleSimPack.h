#ifndef JZ_BLE_SIM_PACK_H_
#define JZ_BLE_SIM_PACK_H_

#include "JZNetPacket.h"
#include "JZVariant.h"
#include <string>
#include <vector>

// 模拟 BLE 通信消息类型
enum JZBleSimMsg {
    SIM_MSG_NONE = 0,

    // Driver -> Device (命令)
    SIM_CMD_SCAN_START,
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

    // Device -> Driver (事件)
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
};

// ============ JZVariantMap 读写辅助 ============
inline int simGetInt(const JZVariantMap &m, const std::string &k, int def = 0)
{
    auto it = m.find(k);
    if (it == m.end()) return def;
    const JZVariant &v = it->second;
    int t = v.typeId();
    if (t == JZMetaType::Int)    return v.toInt();
    if (t == JZMetaType::Int64)  return (int)v.toInt64();
    return def;
}
inline std::string simGetStr(const JZVariantMap &m, const std::string &k, const std::string &def = "")
{
    auto it = m.find(k);
    if (it == m.end()) return def;
    const JZVariant &v = it->second;
    return v.typeId() == JZMetaType::String ? v.toString() : def;
}
inline void simSetInt(JZVariantMap &m, const std::string &k, int v)
{
    m[k] = JZVariant(v);
}
inline void simSetStr(JZVariantMap &m, const std::string &k, const std::string &v)
{
    m[k] = JZVariant(v);
}
// 字符串列表 <-> 单字符串(';' 分隔)
inline void simSetStrs(JZVariantMap &m, const std::string &k, const std::vector<std::string> &v)
{
    std::string s;
    for (size_t i = 0; i < v.size(); i++) { if (i) s += ';'; s += v[i]; }
    m[k] = JZVariant(s);
}
inline std::vector<std::string> simGetStrs(const JZVariantMap &m, const std::string &k)
{
    std::vector<std::string> out;
    std::string s = simGetStr(m, k);
    if (s.empty()) return out;
    size_t pos = 0;
    while (true) {
        size_t n = s.find(';', pos);
        if (n == std::string::npos) { out.push_back(s.substr(pos)); break; }
        out.push_back(s.substr(pos, n - pos));
        pos = n + 1;
    }
    return out;
}

// 模拟 BLE 通信用网络包。整包 payload 为一个 JZVariantMap,
// 其中 "msg" 字段表示消息类型, 其余字段为各消息的参数。
class JZBleSimPack : public JZNetPack
{
public:
    JZBleSimPack();
    int packetType() const override;
    void saveToStream(JZDataStream &s) const override;
    void loadFromStream(JZDataStream &s) override;

    // 确保包类型已注册 (幂等)
    static void ensureRegistered();

    JZVariantMap map;

    int          msg() const { return simGetInt(map, "msg"); }
};

#endif // !JZ_BLE_SIM_PACK_H_
