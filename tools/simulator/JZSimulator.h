#ifndef JZ_SIMULATOR_H_
#define JZ_SIMULATOR_H_

#include "JZObject.h"
#include "JZNetServer.h"
#include "JZBleSimPack.h"
#include <string>
#include <vector>

// 模拟 BLE 设备: 模型/控制器层
// 负责网络监听与 BLE 仿真逻辑, 不依赖任何 UI 框架。
// 界面改由 JZHttpServer + 网页提供: 网页通过 HTTP API 轮询状态/日志,
// 并下发命令。JZSimulator 只需维护状态并提供 JSON 序列化。
class JZSimulator : public JZObject
{
    JZ_OBJECT_DECLARE(JZSimulator)

public:
    // GATT 特征值
    struct SimChar {
        std::string svc, chr;
        int  props = 0;
        std::string value;
        bool notifying = false;
    };

    JZSimulator(JZObject *parent = nullptr);
    ~JZSimulator() override;

    // 启动网络监听 (界面由 Web 服务单独提供)
    void start(int port = 19123);

    // ---- 数据访问 (Web 读取以渲染界面) ----
    const std::string& deviceName() const { return m_name; }
    bool isAdvertising() const { return m_advertising; }
    const std::vector<SimChar>& chars() const { return m_chars; }
    const std::vector<SimChar>& periphChars() const { return m_periphChars; }
    const std::string& periphName() const { return m_periphName; }
    bool hasCentral() const { return !m_centralAddr.empty(); }
    bool hasPeriphClient() const { return !m_periphClientAddr.empty(); }
    std::string statusText() const;

    // ---- Web API: JSON 序列化 ----
    // 完整状态 (设备名/广播/特征/对端特征等)
    std::string stateJson() const;
    // 从 since 索引开始的日志, 以及当前日志总数
    std::string logsJson(size_t since) const;

    // ---- 操作命令 (Web 调用) ----
    void setDeviceName(const std::string& name);
    void setAdvertising(bool on);
    void notifyChar(size_t index, const std::string& value);
    void connectCentral();
    void disconnectCentral();
    void writePeriphChar(size_t index, const std::string& value);

private:
    // ---- 服务端 ----
    void onAccept();
    void onDisconnect(uint64_t c);
    void onPacket(uint64_t c, JZNetPackPtr pack);
    void handlePack(uint64_t c, const JZVariantMap &map);
    void sendToDriver(int msg, const JZVariantMap &m);

    void log(const std::string &text);

    // ---- 网络 ----
    JZNetServer *m_server = nullptr;
    uint64_t     m_clientConn = 0;

    // ---- 日志缓冲 (供网页轮询) ----
    std::vector<std::string> m_logs;

    // ---- Device as Peripheral ----
    std::string m_devAddr = "AA:BB:CC:DD:EE:FF";
    std::string m_name    = "JZSimDev";
    bool        m_advertising = false;
    std::string m_centralAddr;
    std::vector<SimChar> m_chars;

    // ---- Device as Central ----
    std::string m_periphClientAddr;
    std::string m_periphName;
    std::vector<SimChar> m_periphChars;
};

#endif // !JZ_SIMULATOR_H_
