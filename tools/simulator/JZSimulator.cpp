#include "JZSimulator.h"
#include "JZBleSimPack.h"
#include "JZDataStream.h"
#include "JZSignal.h"
#include "JZThread.h"
#include "JZStd.h"
#include "JZNetClient.h"

#include <algorithm>
#include <QString>

JZ_OBJECT_IMPL(JZSimulator, JZObject)

static bool contains(const std::vector<std::string> &v, const std::string &u)
{
    return std::find(v.begin(), v.end(), u) != v.end();
}

JZSimulator::JZSimulator(JZObject *parent) : JZObject(parent)
{
    JZBleSimPack::ensureRegistered();

    // 预置 GATT 表 (Device as Peripheral)
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

JZSimulator::~JZSimulator()
{
    if (m_server) { m_server->close(); delete m_server; m_server = nullptr; }
}

// ======================== 启动 ========================
void JZSimulator::start(int port)
{
    m_server = new JZNetServer(this);
    connect(&m_server->sigAccept,       this, &JZSimulator::onAccept);
    connect(&m_server->sigDisconnect,   this, &JZSimulator::onDisconnect);
    connect(&m_server->sigPacketReceive, this, &JZSimulator::onPacket);
    m_server->listen(port);

    log("BLE Simulator started, listening on port " + std::to_string(port));
}

// ======================== 数据 / 状态 ========================
std::string JZSimulator::statusText() const
{
    std::string s;
    if (!m_periphClientAddr.empty()) s += "App Peripheral connected | ";
    if (!m_centralAddr.empty())       s += "App Central connected";
    if (s.empty()) s = "idle";
    return s;
}

// ======================== 操作命令 ========================
void JZSimulator::setDeviceName(const std::string& name)
{
    m_name = name;
    log("Device name -> " + m_name);
}

void JZSimulator::setAdvertising(bool on)
{
    m_advertising = on;
    log(on ? "Device advertising ON" : "Device advertising OFF");
    notifyStateChanged();
}

void JZSimulator::notifyChar(size_t index, const std::string& value)
{
    if (index >= m_chars.size()) return;
    if (m_centralAddr.empty()) { log("Notify: no App Central connected"); return; }

    m_chars[index].value = value;
    const SimChar& ch = m_chars[index];
    JZVariantMap m;
    simSetStr(m, "addr", m_centralAddr);
    simSetStr(m, "svc", ch.svc);
    simSetStr(m, "chr", ch.chr);
    simSetStr(m, "val", value);
    sendToDriver(SIM_EVT_NOTIFY_RX, m);
    log("Notify " + ch.chr + " = " + value);
}

void JZSimulator::connectCentral()
{
    if (!m_periphClientAddr.empty()) { log("Already connected to App Peripheral"); return; }
    m_periphClientAddr = "11:22:33:44:55:66";
    JZVariantMap m; simSetStr(m, "addr", m_periphClientAddr); simSetInt(m, "role", 1);
    sendToDriver(SIM_EVT_CONNECTED, m);
    log("Connected to App Peripheral (simulated central)");
    notifyStateChanged();
}

void JZSimulator::disconnectCentral()
{
    if (m_periphClientAddr.empty()) return;
    JZVariantMap m; simSetStr(m, "addr", m_periphClientAddr); simSetInt(m, "role", 1);
    sendToDriver(SIM_EVT_DISCONNECTED, m);
    log("Disconnected from App Peripheral");
    m_periphClientAddr.clear();
    notifyStateChanged();
}

void JZSimulator::writePeriphChar(size_t index, const std::string& value)
{
    if (index >= m_periphChars.size()) return;
    if (m_periphClientAddr.empty()) { log("Write: not connected to App Peripheral"); return; }

    m_periphChars[index].value = value;
    const SimChar& ch = m_periphChars[index];
    JZVariantMap m; simSetStr(m, "chr", ch.chr); simSetStr(m, "val", value);
    sendToDriver(SIM_EVT_CHAR_WRITTEN, m);
    log("Write to App Peripheral: " + ch.chr + " = " + value);
}

// ======================== 网络 ========================
void JZSimulator::onAccept()
{
    uint64_t conn = m_server->nextPendingConnection();
    if (conn == 0) return;

    // 单客户端限制: 已有连接则拒绝新连接, 返回 busy 后关闭
    if (m_clientConn != 0) {
        JZVariantMap m;
        simSetInt(m, "msg", SIM_EVT_BUSY);
        JZBleSimPack pack;
        pack.map = m;
        m_server->sendPacket(conn, &pack);
        m_server->closeClient(conn);
        log("rejected new connection (busy): a client is already connected");
        return;
    }

    m_clientConn = conn;
    log("driver connected");
    notifyStateChanged();
}

void JZSimulator::onDisconnect(uint64_t c)
{
    if (c == m_clientConn) {
        m_clientConn = 0;
        m_centralAddr.clear();
        m_periphClientAddr.clear();
        m_periphName.clear();
        m_periphChars.clear();
        log("driver disconnected");
        notifyStateChanged();
        if (m_bridge) emit m_bridge->periphCharsChanged();
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

void JZSimulator::log(const std::string &t)
{
    if (m_bridge) emit m_bridge->logAppended(QString::fromStdString(t));
}

void JZSimulator::notifyStateChanged()
{
    if (m_bridge) emit m_bridge->stateChanged();
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
        notifyStateChanged();
        break;
    }
    case SIM_CMD_DISCONNECT: {
        std::string addr = simGetStr(map, "addr");
        if (addr == m_centralAddr) {
            m_centralAddr.clear();
            JZVariantMap m; simSetStr(m, "addr", addr); simSetInt(m, "role", 0);
            sendToDriver(SIM_EVT_DISCONNECTED, m);
            log("App Central disconnected: " + addr);
            notifyStateChanged();
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
        notifyStateChanged();
        if (m_bridge) emit m_bridge->periphCharsChanged();
        break;
    }
    case SIM_CMD_STOP_ADV:
        log("App Peripheral stopped advertising");
        notifyStateChanged();
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
