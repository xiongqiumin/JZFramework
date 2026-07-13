#ifndef JZ_SIMULATOR_DEVICE_H_
#define JZ_SIMULATOR_DEVICE_H_

#include "JZObject.h"
#include "JZNetServer.h"
#include "JZSimPack.h"
#include <string>
#include <vector>

class JZMainWindow;
class JZLabel;
class JZTextEdit;
class JZSwitch;
class JZPushButton;
class JZWidget;

// 模拟 BLE 设备: 运行 JZNetServer 接收 JZBleDriverSimulator 的指令,
// 并通过 LVGL 界面提供 BLE 相关设置(设备名、广播、特征值、通知/写入等)。
class JZSimulatorDevice : public JZObject
{
    JZ_OBJECT_DECLARE(JZSimulatorDevice)

public:
    JZSimulatorDevice(JZObject *parent = nullptr);
    ~JZSimulatorDevice() override;

    // 创建界面并启动网络服务。port 为监听端口(应用侧 JZBleDriverSimulator 需连接同一端口)。
    void run(int port = 19123);

private:
    void onAccept();
    void onDisconnect(uint64_t c);
    void onPacket(uint64_t c, JZNetPackPtr pack);
    void handlePack(uint64_t c, const JZSimPack &p);

    void sendToDriver(JZSimCmd cmd, const JZVariantMap &m);
    void log(const std::string &text);
    void updateStatus();

    void onSimulateCentralConnect();
    void onDisconnectClient();
    void onNotify(CharRow r);
    void onWrite(CharRow r);

    // 设备自身维护的 GATT 表(供"作为 peripheral 被 App 的 central 连接"场景使用)
    struct SimChar {
        std::string svc;
        std::string chr;
        int        props = 0;
        std::string value;
        bool       notifying = false;
    };
    // 界面中每个特征对应的控件集合
    struct CharRow {
        SimChar    *ch = nullptr;
        JZTextEdit *valueEdit = nullptr;
    };

    JZNetServer *m_server = nullptr;
    uint64_t     m_clientConn = 0;

    std::string m_devAddr = "AA:BB:CC:DD:EE:FF";
    std::string m_name    = "JZSimDev";
    bool        m_advertising = false;
    std::string m_centralAddr;        // App 作为 central 连上来时的地址
    std::string m_periphClientAddr;   // 设备模拟 central 连到 App peripheral 时的地址
    std::vector<SimChar> m_chars;

    // UI
    JZMainWindow *m_win = nullptr;
    JZTextEdit   *m_nameEdit = nullptr;
    JZSwitch     *m_advSwitch = nullptr;
    JZLabel      *m_statusLabel = nullptr;
    JZTextEdit   *m_logEdit = nullptr;
    std::vector<CharRow> m_rows;
};

#endif // !JZ_SIMULATOR_DEVICE_H_
