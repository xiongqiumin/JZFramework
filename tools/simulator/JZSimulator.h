#ifndef JZ_SIMULATOR_H_
#define JZ_SIMULATOR_H_

#include "JZObject.h"
#include "JZNetServer.h"
#include "JZBleSimPack.h"
#include <string>
#include <vector>

// bridge 需要 Qt 支持 (QObject/moc); JZSimulator 主体仅前向声明 bridge 指针, 不依赖 Qt
#include <QObject>
#include <QString>

// 前向声明: 桥接器 (Qt 信号桥, 见文件末尾)
class JZSimulatorBridge;

// 模拟 BLE 设备: 模型/控制器层
// 负责网络监听与 BLE 仿真逻辑, 不依赖任何 UI 框架。
// 界面由独立的 MainWindow (Qt) 负责, 通过 JZSimulatorBridge (QObject)
// 把模型事件以 Qt 信号方式安全地 (跨线程排队) 派发到 UI 线程。
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

    // 设置桥接器 (由 main 创建并绑定), 模型通过它发出 Qt 信号
    void setBridge(JZSimulatorBridge *bridge) { m_bridge = bridge; }

    // 启动网络监听 (UI 由 MainWindow 单独创建)
    void start(int port = 19123);

    // ---- 数据访问 (View 读取以渲染界面) ----
    const std::string& deviceName() const { return m_name; }
    bool isAdvertising() const { return m_advertising; }
    const std::vector<SimChar>& chars() const { return m_chars; }
    const std::vector<SimChar>& periphChars() const { return m_periphChars; }
    const std::string& periphName() const { return m_periphName; }
    bool hasCentral() const { return !m_centralAddr.empty(); }
    bool hasPeriphClient() const { return !m_periphClientAddr.empty(); }
    std::string statusText() const;

    // ---- 操作命令 (View 调用) ----
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
    void notifyStateChanged();

    // ---- 网络 ----
    JZNetServer *m_server = nullptr;
    uint64_t     m_clientConn = 0;

    // ---- 桥接器 (Qt 信号桥, 不持有所有权) ----
    JZSimulatorBridge *m_bridge = nullptr;

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

// ======================== Qt 桥接器 ========================
// 继承 QObject, 负责把 JZSimulator (可能在 JZ 后台/事件线程) 的模型事件
// 以 Qt 信号形式排队派发到 UI 主线程。JZSimulator 本身不依赖 Qt。
class JZSimulatorBridge : public QObject
{
    Q_OBJECT

public:
    explicit JZSimulatorBridge(QObject *parent = nullptr) : QObject(parent) {}

signals:
    // 追加日志行
    void logAppended(const QString &text);
    // 状态/普通特征变化 (界面需刷新 Peripheral 页状态)
    void stateChanged();
    // App Peripheral 特征列表变化 (界面需重建 Central 页列表)
    void periphCharsChanged();
};

#endif // !JZ_SIMULATOR_H_
