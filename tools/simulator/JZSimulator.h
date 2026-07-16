#ifndef JZ_SIMULATOR_H_
#define JZ_SIMULATOR_H_

#include "JZObject.h"
#include "JZNetServer.h"
#include "JZBleSimPack.h"
#include <string>
#include <vector>

// 前向声明 Qt 控件
class QMainWindow;
class QWidget;
class QStackedWidget;
class QLabel;
class QLineEdit;
class QTextEdit;
class QCheckBox;
class QPushButton;
class QVBoxLayout;

// 模拟 BLE 设备: JZNetServer 接收 JZBleDriverSimulator 指令, Qt 界面支持两个角色
class JZSimulator : public JZObject
{
    JZ_OBJECT_DECLARE(JZSimulator)

public:
    // ---- 嵌套类型 (声明前置于所有方法) ----
    struct SimChar {
        std::string svc, chr;
        int  props = 0;
        std::string value;
        bool notifying = false;
    };
    struct CharRow {
        SimChar    *ch = nullptr;
        QLineEdit  *valueEdit = nullptr;
    };
    struct PeriphCharRow {
        SimChar    *ch = nullptr;
        QLineEdit  *valueEdit = nullptr;
    };

    JZSimulator(JZObject *parent = nullptr);
    ~JZSimulator() override;
    void run(int port = 19123);

private:
    // ---- 服务端 ----
    void onAccept();
    void onDisconnect(uint64_t c);
    void onPacket(uint64_t c, JZNetPackPtr pack);
    void handlePack(uint64_t c, const JZVariantMap &map);
    void sendToDriver(int msg, const JZVariantMap &m);

    // ---- UI 构建 ----
    void buildUI();
    void buildPeripheralPage();
    void buildCentralPage();
    void buildConfigPage();
    void switchTab(int idx);

    // ---- Peripheral 页操作 ----
    void onDeviceNotify(CharRow r);

    // ---- Central 页操作 ----
    void onSimulateCentralConnect();
    void onDisconnectPeriphClient();
    void onPeriphWrite(PeriphCharRow r);
    void rebuildPeriphCharUI();

    void log(const std::string &text);
    void updateStatus();

    // ---- 网络 ----
    JZNetServer *m_server = nullptr;
    uint64_t     m_clientConn = 0;

    // ---- Device as Peripheral ----
    std::string m_devAddr = "AA:BB:CC:DD:EE:FF";
    std::string m_name    = "JZSimDev";
    bool        m_advertising = false;
    std::string m_centralAddr;
    std::vector<SimChar> m_chars;
    std::vector<CharRow> m_rows;

    // ---- Device as Central ----
    std::string m_periphClientAddr;
    std::string m_periphName;
    std::vector<SimChar> m_periphChars;
    std::vector<PeriphCharRow> m_periphRows;

    // ---- UI 控件 (Qt) ----
    QMainWindow     *m_win = nullptr;
    QWidget         *m_central = nullptr;
    QStackedWidget  *m_stack = nullptr;
    QWidget         *m_pagePeripheral = nullptr;
    QWidget         *m_pageCentral    = nullptr;
    QWidget         *m_pageConfig     = nullptr;
    std::vector<QPushButton *> m_tabBtns;

    QLineEdit   *m_nameEdit = nullptr;
    QCheckBox   *m_advSwitch = nullptr;
    QWidget     *m_charContainer = nullptr;
    QVBoxLayout *m_charLayout = nullptr;
    QWidget     *m_periphCharContainer = nullptr;
    QVBoxLayout *m_periphCharLayout = nullptr;

    // 全局底部区域
    QLabel      *m_statusLabel = nullptr;
    QTextEdit   *m_logEdit = nullptr;
};

#endif // !JZ_SIMULATOR_H_
