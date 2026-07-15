#include "JZSimulator.h"
#include "JZBleSimPack.h"
#include "JZDataStream.h"
#include "JZSignal.h"
#include "JZThread.h"
#include "JZStd.h"

#include "JZMainWindow.h"
#include "JZWidget.h"
#include "JZLayout.h"
#include "JZLabel.h"
#include "JZTextEdit.h"
#include "JZSwitch.h"
#include "JZPushButton.h"
#include "JZNetClient.h"

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

    m_pageCentral->hide();
    m_pageConfig->hide();
    switchTab(0);

    m_win->show();
    log("BLE Simulator started, listening on port " + std::to_string(port));
}

// ======================== UI 构建 ========================
void JZSimulator::buildUI()
{
    m_win = new JZMainWindow(nullptr, 780, 700, "JZ BLE Simulator");

    // 根: 垂直布局 — 上面是标签区, 下面是全局日志
    auto *root = new JZWidget(m_win);
    root->resize(780, 700);
    root->setLayout(new JZVBoxLayout(0, 0));

    // ---- 上部: 水平 — 左标签 + 右内容 ----
    auto *topArea = new JZWidget(root);
    topArea->resize(780, 520);
    topArea->setLayout(new JZHBoxLayout(0, 0));

    // 左标签
    m_leftPanel = new JZWidget(topArea);
    m_leftPanel->resize(180, 520);
    m_leftPanel->setLayout(new JZVBoxLayout(0, 0));

    auto *tabPeriph = new JZPushButton(m_leftPanel); tabPeriph->setText("BLE Peripheral");
    tabPeriph->resize(180, 50);
    auto *tabCent   = new JZPushButton(m_leftPanel); tabCent->setText("BLE Central");
    tabCent->resize(180, 50);
    auto *tabConf   = new JZPushButton(m_leftPanel); tabConf->setText("Config");
    tabConf->resize(180, 50);
    auto *spacer    = new JZWidget(m_leftPanel); spacer->resize(180, 370);

    m_tabBtns = {tabPeriph, tabCent, tabConf};
    connect(&tabPeriph->sigClicked, this, [this]() { switchTab(0); });
    connect(&tabCent->sigClicked,   this, [this]() { switchTab(1); });
    connect(&tabConf->sigClicked,   this, [this]() { switchTab(2); });

    // 右堆叠
    m_rightStack = new JZWidget(topArea);
    m_rightStack->resize(600, 520);
    m_rightStack->setLayout(new JZVBoxLayout(0, 0));

    buildPeripheralPage();
    buildCentralPage();
    buildConfigPage();

    // ---- 下部: 全局状态 + 日志 ----
    auto *bottomArea = new JZWidget(root);
    bottomArea->resize(780, 180);
    bottomArea->setLayout(new JZVBoxLayout(4, 4));

    auto *statusRow = new JZWidget(bottomArea); statusRow->setLayout(new JZHBoxLayout(4, 4));
    auto *sl = new JZLabel(statusRow); sl->setText("Status:");
    m_statusLabel = new JZLabel(statusRow);
    m_statusLabel->setText("idle");

    auto *logLabel = new JZLabel(bottomArea); logLabel->setText("Log:");
    m_logEdit = new JZTextEdit(bottomArea);
    m_logEdit->resize(770, 140);
    m_logEdit->setText("");
}

void JZSimulator::buildPeripheralPage()
{
    m_pagePeripheral = new JZWidget(m_rightStack);
    m_pagePeripheral->resize(600, 520);
    m_pagePeripheral->setLayout(new JZVBoxLayout(6, 6));

    auto *title = new JZLabel(m_pagePeripheral);
    title->setText("Device as BLE Peripheral");

    // 设备名
    auto *nameRow = new JZWidget(m_pagePeripheral); nameRow->setLayout(new JZHBoxLayout(4, 4));
    auto *nl = new JZLabel(nameRow); nl->setText("Device Name:");
    m_nameEdit = new JZTextEdit(nameRow); m_nameEdit->setText(m_name); m_nameEdit->resize(200, 28);
    auto *applyBtn = new JZPushButton(nameRow); applyBtn->setText("Apply");
    connect(&applyBtn->sigClicked, this, [this]() {
        m_name = m_nameEdit->text();
        log("Device name -> " + m_name);
    });

    // 广播
    auto *advRow = new JZWidget(m_pagePeripheral); advRow->setLayout(new JZHBoxLayout(4, 4));
    auto *al = new JZLabel(advRow); al->setText("Advertising:");
    m_advSwitch = new JZSwitch(advRow);
    connect(&m_advSwitch->sigToggled, this, [this](bool v) {
        m_advertising = v;
        log(v ? "Device advertising ON" : "Device advertising OFF");
        updateStatus();
    });

    auto *hint = new JZLabel(m_pagePeripheral);
    hint->setText("Notify -> App Central  |  App Central can Read/Write these chars");

    // 特征列表
    m_charContainer = new JZWidget(m_pagePeripheral); m_charContainer->setLayout(new JZVBoxLayout(4, 4));
    for (auto &ch : m_chars) {
        auto *row = new JZWidget(m_charContainer); row->setLayout(new JZHBoxLayout(4, 4));
        auto *hl = new JZLabel(row); hl->setText(ch.chr.substr(0, 8) + "...");
        auto *vl = new JZLabel(row); vl->setText("Val:");
        auto *ve = new JZTextEdit(row); ve->setText(ch.value); ve->resize(120, 28);
        auto *nb = new JZPushButton(row); nb->setText("Notify");
        CharRow r; r.ch = &ch; r.valueEdit = ve;
        connect(&nb->sigClicked, this, [this, r]() { onDeviceNotify(r); });
        m_rows.push_back(r);
    }
}

void JZSimulator::buildCentralPage()
{
    m_pageCentral = new JZWidget(m_rightStack);
    m_pageCentral->resize(600, 520);
    m_pageCentral->setLayout(new JZVBoxLayout(6, 6));

    auto *title = new JZLabel(m_pageCentral);
    title->setText("Device as BLE Central (App Peripheral upstream test)");

    auto *ctrlRow = new JZWidget(m_pageCentral); ctrlRow->setLayout(new JZHBoxLayout(4, 4));
    auto *connBtn = new JZPushButton(ctrlRow); connBtn->setText("Connect to App Peripheral");
    connect(&connBtn->sigClicked, this, &JZSimulator::onSimulateCentralConnect);
    auto *discBtn = new JZPushButton(ctrlRow); discBtn->setText("Disconnect");
    connect(&discBtn->sigClicked, this, &JZSimulator::onDisconnectPeriphClient);

    auto *hint = new JZLabel(m_pageCentral);
    hint->setText("App Peripheral chs appear here, Write tests upstream flow (onBleCharWritten)");

    m_periphCharContainer = new JZWidget(m_pageCentral); m_periphCharContainer->setLayout(new JZVBoxLayout(4, 4));
    auto *empty = new JZLabel(m_periphCharContainer);
    empty->setText("  (waiting for App Peripheral to advertise...)");
}

void JZSimulator::buildConfigPage()
{
    m_pageConfig = new JZWidget(m_rightStack);
    m_pageConfig->resize(600, 520);
    m_pageConfig->setLayout(new JZVBoxLayout(6, 6));

    auto *title = new JZLabel(m_pageConfig);
    title->setText("Config");

    auto *info = new JZLabel(m_pageConfig);
    info->setText("Default GATT chars are defined in JZSimulator::run().\n"
                  "Edit this file to change pre-configured services/characteristics.");
}

void JZSimulator::switchTab(int idx)
{
    if (idx == 0)      { m_pagePeripheral->show(); m_pageCentral->hide(); m_pageConfig->hide(); }
    else if (idx == 1) { m_pagePeripheral->hide(); m_pageCentral->show(); m_pageConfig->hide(); }
    else               { m_pagePeripheral->hide(); m_pageCentral->hide(); m_pageConfig->show(); }
}

// ======================== Peripheral 页操作 ========================
void JZSimulator::onDeviceNotify(CharRow r)
{
    if (m_centralAddr.empty()) { log("Notify: no App Central connected"); return; }
    std::string val = r.valueEdit ? r.valueEdit->text() : r.ch->value;
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
    std::string val = r.valueEdit ? r.valueEdit->text() : r.ch->value;
    r.ch->value = val;
    JZVariantMap m; simSetStr(m, "chr", r.ch->chr); simSetStr(m, "val", val);
    sendToDriver(SIM_EVT_CHAR_WRITTEN, m);
    log("Write to App Peripheral: " + r.ch->chr + " = " + val);
}

void JZSimulator::rebuildPeriphCharUI()
{
    if (!m_periphCharContainer) return;
    delete m_periphCharContainer;
    m_periphCharContainer = new JZWidget(m_pageCentral);
    m_periphCharContainer->setLayout(new JZVBoxLayout(4, 4));
    m_periphRows.clear();

    if (m_periphChars.empty()) {
        auto *ph = new JZLabel(m_periphCharContainer);
        ph->setText("  (App Peripheral chars will appear here)");
    } else {
        auto *pnl = new JZLabel(m_periphCharContainer);
        pnl->setText("App Peripheral: " + m_periphName);
        for (auto &ch : m_periphChars) {
            auto *row = new JZWidget(m_periphCharContainer); row->setLayout(new JZHBoxLayout(4, 4));
            auto *hl = new JZLabel(row); hl->setText(ch.chr.substr(0, 8) + "...");
            auto *vl = new JZLabel(row); vl->setText("Val:");
            auto *ve = new JZTextEdit(row); ve->setText(ch.value); ve->resize(120, 28);
            auto *wb = new JZPushButton(row); wb->setText("Write");
            PeriphCharRow r; r.ch = &ch; r.valueEdit = ve;
            connect(&wb->sigClicked, this, [this, r]() { onPeriphWrite(r); });
            m_periphRows.push_back(r);
        }
    }
    m_periphCharContainer->show();
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
    m_logEdit->setText(m_logEdit->text() + t + "\n");
}

void JZSimulator::updateStatus()
{
    if (!m_statusLabel) return;
    std::string s;
    if (!m_periphClientAddr.empty()) s += "App Peripheral connected | ";
    if (!m_centralAddr.empty()) s += "App Central connected";
    if (s.empty()) s = "idle";
    m_statusLabel->setText(s);
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
