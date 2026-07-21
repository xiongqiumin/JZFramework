#include "JZBleDriverSimulator.h"
#include "JZSimPack.h"
#include "JZBLEPeripheral.h"
#include "JZBLEController.h"
#include "JZBLEService.h"
#include "JZBLEDeviceDiscoveryAgent.h"
#include "JZSignal.h"
#include "JZApplication.h"
#include "JZStd.h"
#include "JZByteArray.h"

#include <algorithm>

static bool contains(const std::vector<std::string> &v, const std::string &u)
{
    return std::find(v.begin(), v.end(), u) != v.end();
}

JZBleDriverSimulator::JZBleDriverSimulator(const std::string &ip, int port)
{
    JZSimPack::ensureRegistered();

    m_client = new JZNetClient();
    connect(&m_client->sigPacketReceive, [this](JZNetPackPtr p) { onPacket(p); });
    m_client->connectToHost(ip, port);
    if (jzApp)
        m_client->waitForConnected(500);
}

JZBleDriverSimulator::~JZBleDriverSimulator() {}

void JZBleDriverSimulator::sendSim(JZSimCmd cmd, const JZVariantMap &m)
{
    if (!m_client || !m_client->isConnect())
        return;
    JZSimPack pack;
    pack.cmd = cmd;
    pack.data = m;
    m_client->sendPacket(&pack);
}

int JZBleDriverSimulator::getConnHandle(const std::string &a)
{
    auto it = m_connMap.find(a);
    return it != m_connMap.end() ? it->second : -1;
}

// ===================== Central: 扫描 =====================
void JZBleDriverSimulator::startScan(JZBLEDeviceDiscoveryAgent *agent)
{
    m_scanAgent = agent->guardPointer();
    m_scanning = true;
    JZVariantMap m;
    sendSim(SIM_CMD_SCAN_START, m);
}
void JZBleDriverSimulator::stopScan(JZBLEDeviceDiscoveryAgent *agent)
{
    JZ_ASSERT_X(agent == m_scanAgent.object(), "stopScan invalid param");
    m_scanning = false;
    m_scanAgent = JZObjectPointer();
    JZVariantMap m;
    sendSim(SIM_CMD_SCAN_STOP, m);
}
bool JZBleDriverSimulator::isScanning(JZBLEDeviceDiscoveryAgent *agent)
{
    return agent ? (agent == (JZBLEDeviceDiscoveryAgent *)m_scanAgent.object()) : false;
}

// ===================== Central: 连接 =====================
void JZBleDriverSimulator::connectToDevice(JZBLEController *d, const std::vector<JZBleBondKey>& keys)
{
    std::string addr = d->deviceInfo().address();
    m_centralByAddr[addr] = d;
    JZVariantMap m; m["addr"] = JZVariant(addr);
    sendSim(SIM_CMD_CONNECT, m);
}
void JZBleDriverSimulator::disconnectFromDevice(JZBLEController *d)
{
    if (m_central == d) m_central = nullptr;
    std::string addr = d->deviceInfo().address();
    JZVariantMap m; m["addr"] = JZVariant(addr);
    sendSim(SIM_CMD_DISCONNECT, m);
}
bool JZBleDriverSimulator::isConnected(JZBLEController *d)
{
    return getConnHandle(d->deviceInfo().address()) >= 0;
}

// ===================== Central: 服务/特征发现 =====================
void JZBleDriverSimulator::discoverServices(const std::string &address, JZBLEController *d)
{
    m_svcDiscoverCtrl = d;
    m_svcDiscoverAddr = address;
    JZVariantMap m; m["addr"] = JZVariant(address);
    sendSim(SIM_CMD_DISCOVER_SVC, m);
}
std::vector<JZBluetoothUuid> JZBleDriverSimulator::services(const std::string &address)
{
    auto it = m_svcCache.find(address);
    return it != m_svcCache.end() ? it->second : std::vector<JZBluetoothUuid>();
}

void JZBleDriverSimulator::discoverCharacteristics(const std::string &address, const std::string &serviceUuid,
                                                   JZBLEService *d)
{
    m_chrDiscoverCtrl = d;
    m_chrDiscoverAddr = address;
    m_chrDiscoverSvc  = serviceUuid;
    JZVariantMap m; m["addr"] = JZVariant(address); m["svc"] = JZVariant(serviceUuid);
    sendSim(SIM_CMD_DISCOVER_CHR, m);
}
std::vector<JZLowEnergyCharacteristic> JZBleDriverSimulator::characteristics(const std::string &address,
                                                                            const std::string &serviceUuid)
{
    auto it = m_charCache.find(address + "|" + serviceUuid);
    return it != m_charCache.end() ? it->second : std::vector<JZLowEnergyCharacteristic>();
}

// ===================== Central: 读写/通知 =====================
void JZBleDriverSimulator::readCharacteristic(const std::string &address, const std::string &serviceUuid,
                                              const std::string &charUuid, JZBLEService *d)
{
    auto it = m_charValHandle.find(address + "|" + serviceUuid);
    if (it == m_charValHandle.end()) return;
    auto it2 = it->second.find(charUuid);
    if (it2 == it->second.end()) return;
    int h = it2->second;
    Ctx cx; cx.addr = address; cx.svc = serviceUuid; cx.chr = charUuid; cx.d = d;
    m_readCtx[h] = cx;
    JZVariantMap m; m["addr"] = JZVariant(address); m["svc"] = JZVariant(serviceUuid); m["chr"] = JZVariant(charUuid);
    sendSim(SIM_CMD_READ, m);
}
void JZBleDriverSimulator::writeCharacteristic(const std::string &address, const std::string &serviceUuid,
                                               const std::string &charUuid, const JZByteArray &value,
                                               bool writeNoResponse, JZBLEService *d)
{
    auto it = m_charValHandle.find(address + "|" + serviceUuid);
    if (it == m_charValHandle.end()) return;
    auto it2 = it->second.find(charUuid);
    if (it2 == it->second.end()) return;
    int h = it2->second;
    Ctx cx; cx.addr = address; cx.svc = serviceUuid; cx.chr = charUuid; cx.value = value; cx.d = d;
    m_writeCtx[h] = cx;
    JZVariantMap m;
    m["addr"] = JZVariant(address); m["svc"] = JZVariant(serviceUuid); m["chr"] = JZVariant(charUuid);
    m["val"] = JZVariant(str_fromUtf8(value)); m["nrs"] = JZVariant(writeNoResponse ? 1 : 0);
    sendSim(SIM_CMD_WRITE, m);
}
void JZBleDriverSimulator::enableNotification(const std::string &address, const std::string &serviceUuid,
                                              const std::string &charUuid, bool enable, JZBLEService *d)
{
    (void)d;
    JZVariantMap m;
    m["addr"] = JZVariant(address); m["svc"] = JZVariant(serviceUuid); m["chr"] = JZVariant(charUuid);
    m["en"] = JZVariant(enable ? 1 : 0);
    sendSim(SIM_CMD_ENABLE_NOTIFY, m);
}
void JZBleDriverSimulator::readDescriptor(const std::string &address, const std::string &serviceUuid,
                                          const std::string &charUuid, const std::string &descUuid,
                                          JZBLEService *d)
{
    JZBLEService::onDescriptorRead(d, address.c_str(), serviceUuid.c_str(), descUuid.c_str(), JZByteArray());
}
void JZBleDriverSimulator::writeDescriptor(const std::string &address, const std::string &serviceUuid,
                                           const std::string &charUuid, const std::string &descUuid,
                                           const JZByteArray &value, JZBLEService *d)
{
    JZBLEService::onDescriptorWritten(d, address.c_str(), serviceUuid.c_str(), descUuid.c_str(), value, true);
}

// ===================== Peripheral =====================
void JZBleDriverSimulator::addPeripheralCharacteristic(const std::string &svcUuid, const std::string &charUuid, int properties)
{
    m_periphChars.push_back({svcUuid, charUuid, properties});
    JZVariantMap m; m["svc"] = JZVariant(svcUuid); m["chr"] = JZVariant(charUuid); m["props"] = JZVariant(properties);
    sendSim(SIM_CMD_ADD_PERIPH_CHR, m);
}
void JZBleDriverSimulator::startAdvertising(const std::string &name, const std::vector<JZBluetoothUuid> &serviceUuids,
                                            JZBLEPeripheral *d)
{
    m_periphCtrl = d;
    std::vector<std::string> su;
    for (auto &u : serviceUuids) su.push_back(u.toString());
    JZVariantMap m; m["name"] = JZVariant(name); {
    JZVariantList _l; _l.reserve(su.size());
    for (auto &_e : su) _l.push_back(JZVariant(_e));
    m["su"] = JZVariant::fromValue(_l);
}
    sendSim(SIM_CMD_START_ADV, m);
}
void JZBleDriverSimulator::stopAdvertising()
{
    JZVariantMap m;
    sendSim(SIM_CMD_STOP_ADV, m);
    m_periphConnHandle = -1;
    m_periphCtrl = nullptr;
}
void JZBleDriverSimulator::peripheralSendNotify(const JZBluetoothUuid &charUuid, const JZByteArray &value)
{
    JZVariantMap m; m["chr"] = JZVariant(charUuid.toString()); m["val"] = JZVariant(str_fromUtf8(value));
    sendSim(SIM_CMD_SEND_NOTIFY, m);
}
void JZBleDriverSimulator::peripheralSendIndicate(const JZBluetoothUuid &charUuid, const JZByteArray &value)
{
    JZVariantMap m; m["chr"] = JZVariant(charUuid.toString()); m["val"] = JZVariant(str_fromUtf8(value));
    sendSim(SIM_CMD_SEND_INDICATE, m);
}

// ===================== 报文分发 =====================
void JZBleDriverSimulator::onPacket(JZNetPackPtr pack)
{
    auto *sp = dynamic_cast<JZSimPack *>(pack.get());
    if (!sp) return;

    const JZVariantMap &map = sp->data;
    JZSimCmd cmd = sp->cmd;

    switch (cmd) {
    case SIM_EVT_SCAN_RESULT: {
<<<<<<< HEAD
        std::string addr = simGetStr(map, "addr");
        std::string name = simGetStr(map, "name");
        int rssi = simGetInt(map, "rssi");
        std::vector<std::string> su = simGetStrs(map, "su");
        JZBLEDeviceInfo info;
=======
        std::string addr = map.at("addr").toString();
        std::string name = map.at("name").toString();
        int rssi = map.at("rssi").toInt();
        std::vector<std::string> su;
{
    auto _it = map.find("su");
    if (_it != map.end() && _it->second.typeId() == jzMetaTypeId<JZVariantList>()) {
        const JZVariantList &_l = _it->second.valueRef<JZVariantList>();
        for (auto &_e : _l) if (_e.typeId() == JZMetaType::String) su.push_back(_e.toString());
    }
}
        JZBluetoothDeviceInfo info;
>>>>>>> 4be69fd25136706bf9eb4a53b0f2a788b7f1b0cd
        info.setAddress(addr);
        info.setName(name);
        info.setRssi(rssi);
        std::vector<JZBluetoothUuid> uu;
        for (auto &s : su) uu.push_back(JZBluetoothUuid(s));
        info.setServiceUuids(uu);
        if (m_scanAgent)
            JZBLEDeviceDiscoveryAgent::onDeviceDiscovered((JZBLEDeviceDiscoveryAgent *)m_scanAgent.object(), info);
        break;
    }
    case SIM_EVT_CONNECTED: {
        std::string addr = map.at("addr").toString();
        int role = map.at("role").toInt();
        if (role == 0) {
            m_connMap[addr] = 1;
            if (m_central)
                JZBleDriver::centralOnDeviceConnected(m_central, addr.c_str());
        } else {
            m_periphConnHandle = 1;
            if (m_periphCtrl) JZBleDriver::peripheralOnClientConnected(m_periphCtrl);
        }
        break;
    }
    case SIM_EVT_DISCONNECTED: {
        std::string addr = map.at("addr").toString();
        int role = map.at("role").toInt();
        if (role == 0) {
            m_connMap.erase(addr);
            if (m_central)
                JZBleDriver::centralOnDeviceDisconnected(m_central, addr.c_str());
        } else {
            m_periphConnHandle = -1;
            if (m_periphCtrl) JZBleDriver::peripheralOnClientDisconnected(m_periphCtrl);
        }
        break;
    }
    case SIM_EVT_SERVICES: {
        std::string addr = map.at("addr").toString();
        std::vector<std::string> su;
{
    auto _it = map.find("su");
    if (_it != map.end() && _it->second.typeId() == jzMetaTypeId<JZVariantList>()) {
        const JZVariantList &_l = _it->second.valueRef<JZVariantList>();
        for (auto &_e : _l) if (_e.typeId() == JZMetaType::String) su.push_back(_e.toString());
    }
}
        std::vector<JZBluetoothUuid> vu;
        for (auto &s : su) vu.push_back(JZBluetoothUuid(s));
        m_svcCache[addr] = vu;
        if (m_svcDiscoverCtrl)
            JZBleDriver::centralOnServicesDiscovered(m_svcDiscoverCtrl, addr.c_str());
        break;
    }
    case SIM_EVT_CHARACTERISTICS: {
        std::string addr = map.at("addr").toString();
        std::string svc  = map.at("svc").toString();
        std::vector<std::string> cls;
{
    auto _it = map.find("chars");
    if (_it != map.end() && _it->second.typeId() == jzMetaTypeId<JZVariantList>()) {
        const JZVariantList &_l = _it->second.valueRef<JZVariantList>();
        for (auto &_e : _l) if (_e.typeId() == JZMetaType::String) cls.push_back(_e.toString());
    }
}
        std::vector<JZLowEnergyCharacteristic> chars;
        for (auto &e : cls) {
            size_t p = e.find('|');
            if (p == std::string::npos) continue;
            std::string chu = e.substr(0, p);
            int props = std::stoi(e.substr(p + 1));
            JZLowEnergyCharacteristic c;
            c.setUuid(JZBluetoothUuid(chu));
            c.setProperties(props);
            chars.push_back(c);
            int h = ++m_handleCounter;
            m_charValHandle[addr + "|" + svc][chu] = h;
            Ctx cx; cx.addr = addr; cx.svc = svc; cx.chr = chu; cx.d = m_chrDiscoverCtrl;
            m_readCtx[h] = cx;
            m_ntfCtx[h]  = cx;
        }
        m_charCache[addr + "|" + svc] = chars;
        if (m_chrDiscoverCtrl)
            JZBLEService::onCharacteristicsDiscovered(m_chrDiscoverCtrl, addr.c_str(), svc.c_str());
        break;
    }
    case SIM_EVT_READ_RESULT: {
        std::string addr = map.at("addr").toString();
        std::string svc  = map.at("svc").toString();
        std::string chr  = map.at("chr").toString();
        std::string valStr = map.at("val").toString();
        JZByteArray val(valStr.data(), (int)valStr.size());
        int h = 0;
        auto it = m_charValHandle.find(addr + "|" + svc);
        if (it != m_charValHandle.end()) {
            auto it2 = it->second.find(chr);
            if (it2 != it->second.end()) h = it2->second;
        }
        auto cit = m_readCtx.find(h);
        if (cit != m_readCtx.end()) {
            JZBLEService::onCharacteristicRead(cit->second.d, addr.c_str(), svc.c_str(), val);
            m_readCtx.erase(cit);
        }
        break;
    }
    case SIM_EVT_WRITE_RESULT: {
        std::string addr = map.at("addr").toString();
        std::string svc  = map.at("svc").toString();
        std::string chr  = map.at("chr").toString();
        int h = 0;
        auto it = m_charValHandle.find(addr + "|" + svc);
        if (it != m_charValHandle.end()) {
            auto it2 = it->second.find(chr);
            if (it2 != it->second.end()) h = it2->second;
        }
        auto cit = m_writeCtx.find(h);
        if (cit != m_writeCtx.end()) {
            JZBLEService::onCharacteristicWritten(cit->second.d, addr.c_str(), svc.c_str(),
                                                         cit->second.value, true);
            m_writeCtx.erase(cit);
        }
        break;
    }
    case SIM_EVT_NOTIFY_RX: {
        std::string addr = map.at("addr").toString();
        std::string svc  = map.at("svc").toString();
        std::string chr  = map.at("chr").toString();
        std::string valStr = map.at("val").toString();
        JZByteArray val(valStr.data(), (int)valStr.size());
        int h = 0;
        auto it = m_charValHandle.find(addr + "|" + svc);
        if (it != m_charValHandle.end()) {
            auto it2 = it->second.find(chr);
            if (it2 != it->second.end()) h = it2->second;
        }
        auto cit = m_ntfCtx.find(h);
        if (cit != m_ntfCtx.end())
            JZBLEService::onCharacteristicChanged(cit->second.d, addr.c_str(), svc.c_str(), val);
        break;
    }
    case SIM_EVT_CHAR_WRITTEN: {
        std::string chr = map.at("chr").toString();
        std::string val = map.at("val").toString();
        JZByteArray bval(val.data(), (int)val.size());
        if (m_periphCtrl) JZBleDriver::peripheralOnCharWritten(m_periphCtrl, JZBluetoothUuid(chr), bval);
        break;
    }
    default:
        break;
    }
}
