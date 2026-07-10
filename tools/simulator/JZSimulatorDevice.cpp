#include "JZSimulatorDevice.h"
#include "JZBleSimPack.h"
#include "JZDataStream.h"
#include "JZByteArray.h"
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

#include <algorithm>

JZ_OBJECT_IMPL(JZSimulatorDevice, JZObject)

static bool contains(const std::vector<std::string> &v, const std::string &u)
{
    return std::find(v.begin(), v.end(), u) != v.end();
}

JZSimulatorDevice::JZSimulatorDevice(JZObject *parent) : JZObject(parent) {}
JZSimulatorDevice::~JZSimulatorDevice()
{
    if (m_server) { m_server->close(); delete m_server; m_server = nullptr; }
}

void JZSimulatorDevice::run(int port)
{
    JZBleSimPack::ensureRegistered();

    m_server = new JZNetServer(this);
    connect(&m_server->sigAccept,     this, &JZSimulatorDevice::onAccept);
    connect(&m_server->sigDisconnect, this, &JZSimulatorDevice::onDisconnect);
    connect(&m_server->sigPacketReceive, this, &JZSimulatorDevice::onPacket);
    m_server->listen(port);

    // 预置 GATT 表, 供 App 的 central 发现/读写
    m_chars.push_back({"0000FFF0-0000-1000-8000-00805f9b34fb",
                       "0000FFF1-0000-1000-8000-00805f9b34fb", 0x01 | 0x02 | 0x04, "hello"});
    m_chars.push_back({"0000FFF0-0000-1000-8000-00805f9b34fb",
                       "0000FFF2-0000-1000-8000-00805f9b34fb", 0x01 | 0x02, "world"});
    m_chars.push_back({"0000FFB0-0000-1000-8000-00805f9b34fb",
                       "0000FFB1-0000-1000-8000-00805f9b34fb", 0x01 | 0x04, "1234"});

    // ============ 构建 LVGL 界面 ============
    m_win = new JZMainWindow(nullptr, 560, 780, "JZ BLE Simulator");
    auto *root = new JZWidget(m_win);
    root->resize(560, 780);
    root->setLayout(new JZVBoxLayout(6, 6));

    auto *title = new JZLabel(root);
    title->setText("JZ BLE Simulator Device");

    // 设备名
    auto *nameRow = new JZWidget(root); nameRow->setLayout(new JZHBoxLayout(4, 4));
    auto *nl = new JZLabel(nameRow); nl->setText("Name:");
    m_nameEdit = new JZTextEdit(nameRow); m_nameEdit->setText(m_name); m_nameEdit->resize(200, 30);
    auto *applyName = new JZPushButton(nameRow); applyName->setText("Apply");
    connect(&applyName->sigClicked, this, [this]() {
        m_name = m_nameEdit->text();
        log("name -> " + m_name);
    });

    // 广播 + 模拟 central 连接
    auto *advRow = new JZWidget(root); advRow->setLayout(new JZHBoxLayout(4, 4));
    auto *al = new JZLabel(advRow); al->setText("Advertising:");
    m_advSwitch = new JZSwitch(advRow);
    connect(&m_advSwitch->sigToggled, this, [this](bool v) {
        m_advertising = v;
        log(v ? "advertising ON" : "advertising OFF");
        updateStatus();
    });
    auto *connBtn = new JZPushButton(advRow); connBtn->setText("Simulate Central Connect");
    connect(&connBtn->sigClicked, this, &JZSimulatorDevice::onSimulateCentralConnect);
    auto *discBtn = new JZPushButton(advRow); discBtn->setText("Disconnect Client");
    connect(&discBtn->sigClicked, this, &JZSimulatorDevice::onDisconnectClient);

    m_statusLabel = new JZLabel(root);
    m_statusLabel->setText("Status: idle");

    // 特征列表 (Notify↑ = 设备->App的central; Write→ = 设备作为central写App的peripheral)
    auto *charTitle = new JZLabel(root);
    charTitle->setText("Characteristics  (Notify=to App Central,  Write=to App Peripheral):");
    m_charContainer = new JZWidget(root); m_charContainer->setLayout(new JZVBoxLayout(4, 4));
    for (auto &ch : m_chars) {
        auto *row = new JZWidget(m_charContainer); row->setLayout(new JZVBoxLayout(2, 2));
        auto *hl = new JZLabel(row); hl->setText(ch.chr);
        auto *vr = new JZWidget(row); vr->setLayout(new JZHBoxLayout(4, 4));
        auto *vl = new JZLabel(vr); vl->setText("Val:");
        auto *ve = new JZTextEdit(vr); ve->setText(ch.value); ve->resize(160, 28);
        auto *nb = new JZPushButton(vr); nb->setText("Notify↑");
        auto *wb = new JZPushButton(vr); wb->setText("Write→");
        CharRow r; r.ch = &ch; r.valueEdit = ve;
        connect(&nb->sigClicked, this, [this, r]() { onNotify(r); });
        connect(&wb->sigClicked, this, [this, r]() { onWrite(r); });
        m_rows.push_back(r);
    }

    auto *logTitle = new JZLabel(root); logTitle->setText("Log:");
    m_logEdit = new JZTextEdit(root); m_logEdit->resize(540, 170); m_logEdit->setText("");

    m_win->show();
    log("BLE Simulator started, listening on port " + std::to_string(port));
}

void JZSimulatorDevice::onAccept(uint64_t c)
{
    m_clientConn = c;
    log("driver connected");
    updateStatus();
}
void JZSimulatorDevice::onDisconnect(uint64_t c)
{
    if (c == m_clientConn) {
        m_clientConn = 0;
        m_centralAddr.clear();
        m_periphClientAddr.clear();
        log("driver disconnected");
        updateStatus();
    }
}

void JZSimulatorDevice::onPacket(uint64_t c, JZNetPackPtr pack)
{
    auto *sp = dynamic_cast<JZBleSimPack *>(pack.get());
    if (!sp) return;
    // 统一在主线程处理(界面更新安全)
    runOnObject(this, [this, c, pack]() {
        auto *p = static_cast<JZBleSimPack *>(pack.get());
        handlePack(c, p->map);
    });
}

void JZSimulatorDevice::sendToDriver(int msg, const JZVariantMap &m)
{
    if (!m_server) return;
    JZBleSimPack pack;
    pack.map = m;
    simSetInt(pack.map, "msg", msg);
    if (m_clientConn)
        m_server->sendPacket(m_clientConn, &pack);
    else
        m_server->sendPacketToAll(&pack);
}

void JZSimulatorDevice::log(const std::string &t)
{
    if (!m_logEdit) return;
    std::string cur = m_logEdit->text();
    cur += t + "\n";
    m_logEdit->setText(cur);
}

void JZSimulatorDevice::updateStatus()
{
    if (!m_statusLabel) return;
    std::string s = "Status: ";
    s += m_centralAddr.empty() ? "no-central" : ("central=" + m_centralAddr);
    s += m_periphClientAddr.empty() ? "; no-client" : "; client=yes";
    m_statusLabel->setText(s);
}

void JZSimulatorDevice::onSimulateCentralConnect()
{
    if (!m_periphClientAddr.empty()) { log("client already connected"); return; }
    m_periphClientAddr = "11:22:33:44:55:66";
    JZVariantMap m; simSetStr(m, "addr", m_periphClientAddr); simSetInt(m, "role", 1);
    sendToDriver(SIM_EVT_CONNECTED, m);
    log("simulated central connect -> app peripheral");
    updateStatus();
}
void JZSimulatorDevice::onDisconnectClient()
{
    if (m_periphClientAddr.empty()) return;
    JZVariantMap m; simSetStr(m, "addr", m_periphClientAddr); simSetInt(m, "role", 1);
    sendToDriver(SIM_EVT_DISCONNECTED, m);
    log("simulated central disconnect");
    m_periphClientAddr.clear();
    updateStatus();
}

void JZSimulatorDevice::onNotify(CharRow r)
{
    if (m_centralAddr.empty()) { log("Notify: no central connected"); return; }
    std::string val = r.valueEdit ? r.valueEdit->text() : r.ch->value;
    r.ch->value = val;
    JZVariantMap m;
    simSetStr(m, "addr", m_centralAddr); simSetStr(m, "svc", r.ch->svc);
    simSetStr(m, "chr", r.ch->chr);  simSetStr(m, "val", val);
    sendToDriver(SIM_EVT_NOTIFY_RX, m);
    log("Notify↑ " + r.ch->chr + " = " + val);
}
void JZSimulatorDevice::onWrite(CharRow r)
{
    if (m_periphClientAddr.empty()) { log("Write: no app-peripheral client"); return; }
    std::string val = r.valueEdit ? r.valueEdit->text() : r.ch->value;
    r.ch->value = val;
    JZVariantMap m; simSetStr(m, "chr", r.ch->chr); simSetStr(m, "val", val);
    sendToDriver(SIM_EVT_CHAR_WRITTEN, m);
    log("Write→ " + r.ch->chr + " = " + val);
}

// ===================== 指令处理 =====================
void JZSimulatorDevice::handlePack(uint64_t c, const JZVariantMap &map)
{
    int msg = simGetInt(map, "msg");

    switch (msg) {
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
    case SIM_CMD_SCAN_STOP:
        break;
    case SIM_CMD_CONNECT: {
        std::string addr = simGetStr(map, "addr");
        m_centralAddr = addr;
        JZVariantMap m; simSetStr(m, "addr", addr); simSetInt(m, "role", 0);
        sendToDriver(SIM_EVT_CONNECTED, m);
        log("central connected: " + addr);
        updateStatus();
        break;
    }
    case SIM_CMD_DISCONNECT: {
        std::string addr = simGetStr(map, "addr");
        if (addr == m_centralAddr) {
            m_centralAddr.clear();
            JZVariantMap m; simSetStr(m, "addr", addr); simSetInt(m, "role", 0);
            sendToDriver(SIM_EVT_DISCONNECTED, m);
            log("central disconnected: " + addr);
            updateStatus();
        }
        break;
    }
    case SIM_CMD_DISCOVER_SVC: {
        std::string addr = simGetStr(map, "addr");
        std::vector<std::string> su;
        for (auto &ch : m_chars) if (!contains(su, ch.svc)) su.push_back(ch.svc);
        JZVariantMap m; simSetStr(m, "addr", addr); simSetStrs(m, "su", su);
        sendToDriver(SIM_EVT_SERVICES, m);
        break;
    }
    case SIM_CMD_DISCOVER_CHR: {
        std::string addr = simGetStr(map, "addr");
        std::string svc  = simGetStr(map, "svc");
        std::vector<std::string> cls;
        for (auto &ch : m_chars)
            if (ch.svc == svc) cls.push_back(ch.chr + "|" + std::to_string(ch.props));
        JZVariantMap m; simSetStr(m, "addr", addr); simSetStr(m, "svc", svc); simSetStrs(m, "chars", cls);
        sendToDriver(SIM_EVT_CHARACTERISTICS, m);
        break;
    }
    case SIM_CMD_READ: {
        std::string addr = simGetStr(map, "addr");
        std::string svc  = simGetStr(map, "svc");
        std::string chr  = simGetStr(map, "chr");
        std::string val;
        for (auto &ch : m_chars) if (ch.svc == svc && ch.chr == chr) { val = ch.value; break; }
        JZVariantMap m; simSetStr(m, "addr", addr); simSetStr(m, "svc", svc);
        simSetStr(m, "chr", chr); simSetStr(m, "val", val);
        sendToDriver(SIM_EVT_READ_RESULT, m);
        break;
    }
    case SIM_CMD_WRITE: {
        std::string addr = simGetStr(map, "addr");
        std::string svc  = simGetStr(map, "svc");
        std::string chr  = simGetStr(map, "chr");
        std::string val  = simGetStr(map, "val");
        for (auto &ch : m_chars) if (ch.svc == svc && ch.chr == chr) ch.value = val;
        log("central wrote " + chr + " = " + val);
        JZVariantMap m; simSetStr(m, "addr", addr); simSetStr(m, "svc", svc); simSetStr(m, "chr", chr);
        sendToDriver(SIM_EVT_WRITE_RESULT, m);
        break;
    }
    case SIM_CMD_ENABLE_NOTIFY: {
        std::string addr = simGetStr(map, "addr");
        std::string svc  = simGetStr(map, "svc");
        std::string chr  = simGetStr(map, "chr");
        int en = simGetInt(map, "en");
        for (auto &ch : m_chars) if (ch.svc == svc && ch.chr == chr) ch.notifying = (en != 0);
        log(std::string("notify ") + (en ? "enabled" : "disabled") + " " + chr);
        break;
    }
    case SIM_CMD_ADD_PERIPH_CHR: {
        std::string svc   = simGetStr(map, "svc");
        std::string chr   = simGetStr(map, "chr");
        int props = simGetInt(map, "props");
        log("app peripheral added char " + chr);
        break;
    }
    case SIM_CMD_START_ADV: {
        std::string name = simGetStr(map, "name");
        std::vector<std::string> su = simGetStrs(map, "su");
        log("app started advertising as peripheral: " + name);
        updateStatus();
        break;
    }
    case SIM_CMD_STOP_ADV:
        log("app stopped advertising");
        updateStatus();
        break;
    case SIM_CMD_SEND_NOTIFY: {
        std::string chr = simGetStr(map, "chr");
        std::string val = simGetStr(map, "val");
        log("app peripheral notified " + chr + " = " + val);
        break;
    }
    case SIM_CMD_SEND_INDICATE: {
        std::string chr = simGetStr(map, "chr");
        std::string val = simGetStr(map, "val");
        log("app peripheral indicated " + chr + " = " + val);
        break;
    }
    default:
        break;
    }
}
