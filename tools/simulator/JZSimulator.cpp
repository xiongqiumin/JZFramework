#include "JZSimulator.h"
#include "JZBleSimPack.h"
#include "JZDataStream.h"
#include "JZSignal.h"
#include "JZThread.h"
#include "JZStd.h"
#include "JZNetClient.h"

#include <QApplication>
#include <QMainWindow>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QLineEdit>
#include <QTextEdit>
#include <QCheckBox>
#include <QStackedWidget>
#include <QString>

#include <algorithm>

JZ_OBJECT_IMPL(JZSimulator, JZObject)

static bool contains(const std::vector<std::string> &v, const std::string &u)
{
    return std::find(v.begin(), v.end(), u) != v.end();
}

JZSimulator::JZSimulator(JZObject *parent) : JZObject(parent) {}
JZSimulator::~JZSimulator()
{
    if (m_server) { m_server->close(); delete m_server; m_server = nullptr; }
    if (m_win) { delete m_win; m_win = nullptr; }
}

// ======================== 启动 ========================
void JZSimulator::run(int port)
{
    JZBleSimPack::ensureRegistered();

    // 网络
    m_server = new JZNetServer(this);
    connect(&m_server->sigAccept,     this, &JZSimulator::onAccept);
    connect(&m_server->sigDisconnect, this, &JZSimulator::onDisconnect);
    connect(&m_server->sigPacketReceive, this, &JZSimulator::onPacket);
    m_server->listen(port);

    // 预置 GATT 表 (Device as Peripheral)
    {
        SimChar c1; c1.svc = "0000FFF0-0000-1000-8000-00805f9b34fb";
        c1.chr = "0000FFF1-0000-1000-8000-00805f9b34fb";
        c1.props = 0x01 | 0x02 | 0x04; c1.value = "hello"; c1.notifying = false;
        m_chars.push_back(c1);

        SimChar c2; c2.svc = "0000FFF0-0000-1000-8000-00805f9b34fb";
        c2.chr = "0000FFF2-0000-1000-8000-00805f9b34fb";
        c2.props = 0x01 | 0x02; c2.value = "world"; c2.notifying = false;
        m_chars.push_back(c2);

        SimChar c3; c3.svc = "0000FFB0-0000-1000-8000-00805f9b34fb";
        c3.chr = "0000FFB1-0000-1000-8000-00805f9b34fb";
        c3.props = 0x01 | 0x04; c3.value = "1234"; c3.notifying = false;
        m_chars.push_back(c3);
    }

    buildUI();

    switchTab(0);

    m_win->show();
    log("BLE Simulator started, listening on port " + std::to_string(port));
}

// ======================== UI 构建 ========================
void JZSimulator::buildUI()
{
    m_win = new QMainWindow(nullptr);
    m_win->setWindowTitle("JZ BLE Simulator");
    m_win->resize(780, 700);

    m_central = new QWidget(m_win);
    m_win->setCentralWidget(m_central);

    auto *root = new QVBoxLayout(m_central);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(8);

    // ---- 上部: 水平 — 左标签 + 右内容 ----
    auto *topArea = new QWidget(m_central);
    auto *topLayout = new QHBoxLayout(topArea);
    topLayout->setContentsMargins(0, 0, 0, 0);
    topLayout->setSpacing(8);

    // 左标签
    auto *leftPanel = new QWidget(topArea);
    leftPanel->setFixedWidth(180);
    auto *leftLayout = new QVBoxLayout(leftPanel);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(6);

    auto *tabPeriph = new QPushButton("BLE Peripheral", leftPanel);
    auto *tabCent   = new QPushButton("BLE Central", leftPanel);
    auto *tabConf   = new QPushButton("Config", leftPanel);
    leftLayout->addWidget(tabPeriph);
    leftLayout->addWidget(tabCent);
    leftLayout->addWidget(tabConf);
    leftLayout->addStretch(1);

    m_tabBtns = {tabPeriph, tabCent, tabConf};
    QObject::connect(tabPeriph, &QPushButton::clicked, this, [this]() { switchTab(0); });
    QObject::connect(tabCent,   &QPushButton::clicked, this, [this]() { switchTab(1); });
    QObject::connect(tabConf,   &QPushButton::clicked, this, [this]() { switchTab(2); });

    // 右堆叠
    m_stack = new QStackedWidget(topArea);
    buildPeripheralPage();
    buildCentralPage();
    buildConfigPage();
    m_stack->addWidget(m_pagePeripheral);
    m_stack->addWidget(m_pageCentral);
    m_stack->addWidget(m_pageConfig);

    topLayout->addWidget(leftPanel);
    topLayout->addWidget(m_stack, 1);

    root->addWidget(topArea, 1);

    // ---- 下部: 全局状态 + 日志 ----
    auto *bottomArea = new QWidget(m_central);
    auto *bottomLayout = new QVBoxLayout(bottomArea);
    bottomLayout->setContentsMargins(0, 0, 0, 0);
    bottomLayout->setSpacing(4);

    auto *statusRow = new QWidget(bottomArea);
    auto *statusLayout = new QHBoxLayout(statusRow);
    statusLayout->setContentsMargins(0, 0, 0, 0);
    statusLayout->setSpacing(4);
    auto *sl = new QLabel("Status:", statusRow);
    m_statusLabel = new QLabel("idle", statusRow);
    statusLayout->addWidget(sl);
    statusLayout->addWidget(m_statusLabel, 1);

    auto *logLabel = new QLabel("Log:", bottomArea);
    m_logEdit = new QTextEdit(bottomArea);
    m_logEdit->setReadOnly(true);

    bottomLayout->addWidget(statusRow);
    bottomLayout->addWidget(logLabel);
    bottomLayout->addWidget(m_logEdit, 1);

    root->addWidget(bottomArea);
}

void JZSimulator::buildPeripheralPage()
{
    m_pagePeripheral = new QWidget();
    auto *layout = new QVBoxLayout(m_pagePeripheral);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    layout->addWidget(new QLabel("Device as BLE Peripheral"));

    // 设备名
    auto *nameRow = new QWidget(m_pagePeripheral);
    auto *nameLayout = new QHBoxLayout(nameRow);
    nameLayout->setContentsMargins(0, 0, 0, 0);
    nameLayout->setSpacing(4);
    nameLayout->addWidget(new QLabel("Device Name:"));
    m_nameEdit = new QLineEdit(m_pagePeripheral);
    m_nameEdit->setText(QString::fromStdString(m_name));
    nameLayout->addWidget(m_nameEdit);
    auto *applyBtn = new QPushButton("Apply", m_pagePeripheral);
    nameLayout->addWidget(applyBtn);
    nameLayout->addStretch(1);
    QObject::connect(applyBtn, &QPushButton::clicked, this, [this]() {
        m_name = m_nameEdit->text().toStdString();
        log("Device name -> " + m_name);
    });
    layout->addWidget(nameRow);

    // 广播
    auto *advRow = new QWidget(m_pagePeripheral);
    auto *advLayout = new QHBoxLayout(advRow);
    advLayout->setContentsMargins(0, 0, 0, 0);
    advLayout->setSpacing(4);
    advLayout->addWidget(new QLabel("Advertising:"));
    m_advSwitch = new QCheckBox(m_pagePeripheral);
    advLayout->addWidget(m_advSwitch);
    advLayout->addStretch(1);
    QObject::connect(m_advSwitch, &QCheckBox::toggled, this, [this](bool v) {
        m_advertising = v;
        log(v ? "Device advertising ON" : "Device advertising OFF");
        updateStatus();
    });
    layout->addWidget(advRow);

    layout->addWidget(new QLabel("Notify -> App Central  |  App Central can Read/Write these chars"));

    // 特征列表
    m_charContainer = new QWidget(m_pagePeripheral);
    m_charLayout = new QVBoxLayout(m_charContainer);
    m_charLayout->setContentsMargins(0, 0, 0, 0);
    m_charLayout->setSpacing(4);
    for (auto &ch : m_chars) {
        auto *row = new QWidget(m_charContainer);
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(4);
        rowLayout->addWidget(new QLabel(QString::fromStdString(ch.chr.substr(0, 8) + "...")));
        rowLayout->addWidget(new QLabel("Val:"));
        auto *ve = new QLineEdit(m_charContainer);
        ve->setText(QString::fromStdString(ch.value));
        ve->setFixedWidth(120);
        rowLayout->addWidget(ve);
        auto *nb = new QPushButton("Notify", m_charContainer);
        rowLayout->addWidget(nb);
        rowLayout->addStretch(1);

        CharRow r; r.ch = &ch; r.valueEdit = ve;
        QObject::connect(nb, &QPushButton::clicked, this, [this, r]() { onDeviceNotify(r); });
        m_rows.push_back(r);
    }
    layout->addWidget(m_charContainer);
    layout->addStretch(1);
}

void JZSimulator::buildCentralPage()
{
    m_pageCentral = new QWidget();
    auto *layout = new QVBoxLayout(m_pageCentral);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    layout->addWidget(new QLabel("Device as BLE Central (App Peripheral upstream test)"));

    auto *ctrlRow = new QWidget(m_pageCentral);
    auto *ctrlLayout = new QHBoxLayout(ctrlRow);
    ctrlLayout->setContentsMargins(0, 0, 0, 0);
    ctrlLayout->setSpacing(4);
    auto *connBtn = new QPushButton("Connect to App Peripheral", m_pageCentral);
    auto *discBtn = new QPushButton("Disconnect", m_pageCentral);
    ctrlLayout->addWidget(connBtn);
    ctrlLayout->addWidget(discBtn);
    ctrlLayout->addStretch(1);
    QObject::connect(connBtn, &QPushButton::clicked, this, &JZSimulator::onSimulateCentralConnect);
    QObject::connect(discBtn, &QPushButton::clicked, this, &JZSimulator::onDisconnectPeriphClient);
    layout->addWidget(ctrlRow);

    layout->addWidget(new QLabel("App Peripheral chs appear here, Write tests upstream flow (onBleCharWritten)"));

    m_periphCharContainer = new QWidget(m_pageCentral);
    m_periphCharLayout = new QVBoxLayout(m_periphCharContainer);
    m_periphCharLayout->setContentsMargins(0, 0, 0, 0);
    m_periphCharLayout->setSpacing(4);
    m_periphCharLayout->addWidget(new QLabel("  (waiting for App Peripheral to advertise...)"));
    layout->addWidget(m_periphCharContainer);
    layout->addStretch(1);
}

void JZSimulator::buildConfigPage()
{
    m_pageConfig = new QWidget();
    auto *layout = new QVBoxLayout(m_pageConfig);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    layout->addWidget(new QLabel("Config"));
    layout->addWidget(new QLabel("Default GATT chars are defined in JZSimulator::run().\n"
                                 "Edit this file to change pre-configured services/characteristics."));
    layout->addStretch(1);
}

void JZSimulator::switchTab(int idx)
{
    if (idx < 0 || idx > 2) return;
    m_stack->setCurrentIndex(idx);
    for (size_t i = 0; i < m_tabBtns.size(); i++)
        m_tabBtns[i]->setEnabled(i != (size_t)idx);
}

// ======================== Peripheral 页操作 ========================
void JZSimulator::onDeviceNotify(CharRow r)
{
    if (m_centralAddr.empty()) { log("Notify: no App Central connected"); return; }
    std::string val = r.valueEdit ? r.valueEdit->text().toStdString() : r.ch->value;
    r.ch->value = val;
    JZVariantMap m;
    simSetStr(m, "addr", m_centralAddr);
    simSetStr(m, "svc", r.ch->svc);
    simSetStr(m, "chr", r.ch->chr);
    simSetStr(m, "val", val);
    sendToDriver(SIM_EVT_NOTIFY_RX, m);
    log("Notify " + r.ch->chr + " = " + val);
}

// ======================== Central 页操作 ========================
void JZSimulator::onSimulateCentralConnect()
{
    if (!m_periphClientAddr.empty()) { log("Already connected to App Peripheral"); return; }
    m_periphClientAddr = "11:22:33:44:55:66";
    JZVariantMap m; simSetStr(m, "addr", m_periphClientAddr); simSetInt(m, "role", 1);
    sendToDriver(SIM_EVT_CONNECTED, m);
    log("Connected to App Peripheral (simulated central)");
    updateStatus();
}

void JZSimulator::onDisconnectPeriphClient()
{
    if (m_periphClientAddr.empty()) return;
    JZVariantMap m; simSetStr(m, "addr", m_periphClientAddr); simSetInt(m, "role", 1);
    sendToDriver(SIM_EVT_DISCONNECTED, m);
    log("Disconnected from App Peripheral");
    m_periphClientAddr.clear();
    updateStatus();
}

void JZSimulator::onPeriphWrite(PeriphCharRow r)
{
    if (m_periphClientAddr.empty()) { log("Write: not connected to App Peripheral"); return; }
    std::string val = r.valueEdit ? r.valueEdit->text().toStdString() : r.ch->value;
    r.ch->value = val;
    JZVariantMap m; simSetStr(m, "chr", r.ch->chr); simSetStr(m, "val", val);
    sendToDriver(SIM_EVT_CHAR_WRITTEN, m);
    log("Write to App Peripheral: " + r.ch->chr + " = " + val);
}

void JZSimulator::rebuildPeriphCharUI()
{
    if (!m_periphCharLayout) return;

    // 清空旧内容
    while (QLayoutItem *item = m_periphCharLayout->takeAt(0)) {
        if (QWidget *w = item->widget()) delete w;
        delete item;
    }
    m_periphRows.clear();

    if (m_periphChars.empty()) {
        m_periphCharLayout->addWidget(new QLabel("  (App Peripheral chars will appear here)"));
    } else {
        m_periphCharLayout->addWidget(new QLabel(QString::fromStdString("App Peripheral: " + m_periphName)));
        for (auto &ch : m_periphChars) {
            auto *row = new QWidget(m_periphCharContainer);
            auto *rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(0, 0, 0, 0);
            rowLayout->setSpacing(4);
            rowLayout->addWidget(new QLabel(QString::fromStdString(ch.chr.substr(0, 8) + "...")));
            rowLayout->addWidget(new QLabel("Val:"));
            auto *ve = new QLineEdit(m_periphCharContainer);
            ve->setText(QString::fromStdString(ch.value));
            ve->setFixedWidth(120);
            rowLayout->addWidget(ve);
            auto *wb = new QPushButton("Write", m_periphCharContainer);
            rowLayout->addWidget(wb);
            rowLayout->addStretch(1);

            PeriphCharRow r; r.ch = &ch; r.valueEdit = ve;
            QObject::connect(wb, &QPushButton::clicked, this, [this, r]() { onPeriphWrite(r); });
            m_periphRows.push_back(r);
        }
    }
}

// ======================== 网络 ========================
void JZSimulator::onAccept()
{
    JZNetClient* client = m_server->nextPendingConnection();
    if (!client) return;
    m_clientConn = 1; // 单连接场景，固定 ID
    log("driver connected");
    updateStatus();
}

void JZSimulator::onDisconnect(uint64_t c)
{
    if (c == m_clientConn) {
        m_clientConn = 0;
        m_centralAddr.clear();
        m_periphClientAddr.clear();
        m_periphName.clear();
        m_periphChars.clear();
        m_periphRows.clear();
        log("driver disconnected");
        updateStatus();
    }
}

void JZSimulator::onPacket(uint64_t c, JZNetPackPtr pack)
{
    auto *sp = dynamic_cast<JZBleSimPack *>(pack.get());
    if (!sp) return;
    runOnObject(this, [this, c, pack]() {
        auto *p = static_cast<JZBleSimPack *>(pack.get());
        handlePack(c, p->map);
    });
}

void JZSimulator::sendToDriver(int msg, const JZVariantMap &m)
{
    if (!m_server) return;
    JZBleSimPack pack;
    pack.map = m;
    simSetInt(pack.map, "msg", msg);
    m_server->sendPacketToAll(&pack);
}

// ======================== 日志 / 状态 ========================
void JZSimulator::log(const std::string &t)
{
    if (!m_logEdit) return;
    m_logEdit->append(QString::fromStdString(t));
}

void JZSimulator::updateStatus()
{
    if (!m_statusLabel) return;
    std::string s;
    if (!m_periphClientAddr.empty()) s += "App Peripheral connected | ";
    if (!m_centralAddr.empty()) s += "App Central connected";
    if (s.empty()) s = "idle";
    m_statusLabel->setText(QString::fromStdString(s));
}

// ======================== 指令处理 ========================
void JZSimulator::handlePack(uint64_t c, const JZVariantMap &map)
{
    int msg = simGetInt(map, "msg");
    switch (msg) {

    // ---- Central 扫/连/断 ----
    case SIM_CMD_SCAN_START: {
        if (!m_advertising) { log("scan ignored (not advertising)"); break; }
        std::vector<std::string> su;
        for (auto &ch : m_chars) if (!contains(su, ch.svc)) su.push_back(ch.svc);
        JZVariantMap m;
        simSetStr(m, "addr", m_devAddr); simSetStr(m, "name", m_name);
        simSetInt(m, "rssi", -40); simSetStrs(m, "su", su);
        sendToDriver(SIM_EVT_SCAN_RESULT, m);
        log("scan -> reported " + m_name);
        break;
    }
    case SIM_CMD_SCAN_STOP: break;
    case SIM_CMD_CONNECT: {
        m_centralAddr = simGetStr(map, "addr");
        JZVariantMap m; simSetStr(m, "addr", m_centralAddr); simSetInt(m, "role", 0);
        sendToDriver(SIM_EVT_CONNECTED, m);
        log("App Central connected: " + m_centralAddr);
        updateStatus();
        break;
    }
    case SIM_CMD_DISCONNECT: {
        std::string addr = simGetStr(map, "addr");
        if (addr == m_centralAddr) {
            m_centralAddr.clear();
            JZVariantMap m; simSetStr(m, "addr", addr); simSetInt(m, "role", 0);
            sendToDriver(SIM_EVT_DISCONNECTED, m);
            log("App Central disconnected: " + addr);
            updateStatus();
        }
        break;
    }

    // ---- Central: 服务/特征发现 ----
    case SIM_CMD_DISCOVER_SVC: {
        std::vector<std::string> su;
        for (auto &ch : m_chars) if (!contains(su, ch.svc)) su.push_back(ch.svc);
        JZVariantMap m; simSetStr(m, "addr", simGetStr(map, "addr")); simSetStrs(m, "su", su);
        sendToDriver(SIM_EVT_SERVICES, m);
        break;
    }
    case SIM_CMD_DISCOVER_CHR: {
        std::string svc = simGetStr(map, "svc");
        std::vector<std::string> cls;
        for (auto &ch : m_chars)
            if (ch.svc == svc) cls.push_back(ch.chr + "|" + std::to_string(ch.props));
        JZVariantMap m;
        simSetStr(m, "addr", simGetStr(map, "addr")); simSetStr(m, "svc", svc);
        simSetStrs(m, "chars", cls);
        sendToDriver(SIM_EVT_CHARACTERISTICS, m);
        break;
    }

    // ---- Central: 读写/通知 ----
    case SIM_CMD_READ: {
        std::string svc = simGetStr(map, "svc"), chr = simGetStr(map, "chr"), val;
        for (auto &ch : m_chars) if (ch.svc == svc && ch.chr == chr) { val = ch.value; break; }
        JZVariantMap m;
        simSetStr(m, "addr", simGetStr(map, "addr")); simSetStr(m, "svc", svc);
        simSetStr(m, "chr", chr); simSetStr(m, "val", val);
        sendToDriver(SIM_EVT_READ_RESULT, m);
        break;
    }
    case SIM_CMD_WRITE: {
        std::string svc = simGetStr(map, "svc"), chr = simGetStr(map, "chr"),
                    val = simGetStr(map, "val");
        for (auto &ch : m_chars) if (ch.svc == svc && ch.chr == chr) ch.value = val;
        log("central wrote " + chr + " = " + val);
        JZVariantMap m;
        simSetStr(m, "addr", simGetStr(map, "addr")); simSetStr(m, "svc", svc);
        simSetStr(m, "chr", chr);
        sendToDriver(SIM_EVT_WRITE_RESULT, m);
        break;
    }
    case SIM_CMD_ENABLE_NOTIFY: {
        std::string svc = simGetStr(map, "svc"), chr = simGetStr(map, "chr");
        int en = simGetInt(map, "en");
        for (auto &ch : m_chars) if (ch.svc == svc && ch.chr == chr) ch.notifying = (en != 0);
        log(std::string("notify ") + (en ? "enabled" : "disabled") + " " + chr);
        break;
    }

    // ---- Peripheral: 特征注册/广播/通知 ----
    case SIM_CMD_ADD_PERIPH_CHR: {
        std::string svc = simGetStr(map, "svc"), chr = simGetStr(map, "chr");
        int props = simGetInt(map, "props");
        bool exists = false;
        for (auto &ch : m_periphChars) {
            if (ch.chr == chr) { ch.svc = svc; ch.props = props; exists = true; break; }
        }
        if (!exists) { SimChar sc; sc.svc = svc; sc.chr = chr; sc.props = props; sc.value = ""; sc.notifying = false; m_periphChars.push_back(sc); }
        log("App Peripheral char registered: " + chr);
        break;
    }
    case SIM_CMD_START_ADV: {
        m_periphName = simGetStr(map, "name");
        log("App Peripheral advertising: " + m_periphName);
        updateStatus();
        rebuildPeriphCharUI();
        break;
    }
    case SIM_CMD_STOP_ADV:
        log("App Peripheral stopped advertising");
        updateStatus();
        break;
    case SIM_CMD_SEND_NOTIFY: {
        std::string chr = simGetStr(map, "chr"), val = simGetStr(map, "val");
        for (auto &ch : m_periphChars) if (ch.chr == chr) ch.value = val;
        log("App Peripheral Notify: " + chr + " = " + val);
        break;
    }
    case SIM_CMD_SEND_INDICATE: {
        std::string chr = simGetStr(map, "chr"), val = simGetStr(map, "val");
        for (auto &ch : m_periphChars) if (ch.chr == chr) ch.value = val;
        log("App Peripheral Indicate: " + chr + " = " + val);
        break;
    }
    default: break;
    }
}
