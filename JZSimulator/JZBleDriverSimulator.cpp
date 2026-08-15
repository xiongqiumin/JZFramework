#include "JZBleDriverSimulator.h"
#include "JZSimPack.h"
#include "JZBLEPeripheral.h"
#include "JZBLEController.h"
#include "JZBLEService.h"
#include "JZBLEDeviceDiscoveryAgent.h"
#include "JZBLEDeviceInfo.h"
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
    (void)ip;
    (void)port;
    m_sim = JZSimServer::instance();
}

JZBleDriverSimulator::~JZBleDriverSimulator() {}

void JZBleDriverSimulator::sendSim(JZSimCmd cmd, const JZVariantMap &m)
{
    m_sim->sendSim(cmd, m);
}

int JZBleDriverSimulator::getConnHandle(const std::string &a)
{
    auto it = m_connMap.find(a);
    return it != m_connMap.end() ? it->second : -1;
}

JZBleDriverSimulator *JZBleDriverSimulator::simInstance()
{
    return dynamic_cast<JZBleDriverSimulator *>(JZBleDriver::instance());
}


// ===================== Central: 扫描 =====================
void JZBleDriverSimulator::startScan(JZBLEDeviceDiscoveryAgent *agent)
{
    { JZMutexLocker lk(&m_mutex); m_scanAgent = agent->guardPointer(); m_scanning = true; }
    JZVariantMap m;
    sendSim(SIM_CMD_SCAN_START, m);
}
void JZBleDriverSimulator::stopScan(JZBLEDeviceDiscoveryAgent *agent)
{
    { JZMutexLocker lk(&m_mutex);
      JZ_ASSERT_X(agent == m_scanAgent.object(), "stopScan invalid param");
      m_scanning = false;
      m_scanAgent = JZObjectPointer(); }
    JZVariantMap m;
    sendSim(SIM_CMD_SCAN_STOP, m);
}
bool JZBleDriverSimulator::isScanning(JZBLEDeviceDiscoveryAgent *agent)
{
    JZMutexLocker lk(&m_mutex);
    return agent ? (agent == (JZBLEDeviceDiscoveryAgent *)m_scanAgent.object()) : false;
}

// ===================== Central: 连接 =====================
void JZBleDriverSimulator::connectToDevice(JZBLEController *d, const std::vector<JZBleBondKey>& keys)
{
    (void)keys;
    std::string addr = d->deviceInfo().address();
    // 设计上仅支持单个 Central（最近一次 connect 的），连接事件按 m_central 路由；
    // m_central 同时被网络接收线程读取，需加锁。
    {
        JZMutexLocker lk(&m_mutex);
        m_central = d;
    }
    JZVariantMap m; m["addr"] = JZVariant(addr);
    sendSim(SIM_CMD_CONNECT, m);
}
void JZBleDriverSimulator::disconnectFromDevice(JZBLEController *d)
{
    { JZMutexLocker lk(&m_mutex); if (m_central == d) m_central = nullptr; }
    std::string addr = d->deviceInfo().address();
    JZVariantMap m; m["addr"] = JZVariant(addr);
    sendSim(SIM_CMD_DISCONNECT, m);
}
bool JZBleDriverSimulator::isConnected(JZBLEController *d)
{
    JZMutexLocker lk(&m_mutex);
    return getConnHandle(d->deviceInfo().address()) >= 0;
}

// ===================== Central: 服务/特征发现 =====================
void JZBleDriverSimulator::discoverServices(const std::string &address, JZBLEController *d)
{
    { JZMutexLocker lk(&m_mutex); m_svcDiscoverCtrl = d; m_svcDiscoverAddr = address; }
    JZVariantMap m; m["addr"] = JZVariant(address);
    sendSim(SIM_CMD_DISCOVER_SVC, m);
}
std::vector<JZBluetoothUuid> JZBleDriverSimulator::services(const std::string &address)
{
    JZMutexLocker lk(&m_mutex);
    auto it = m_svcCache.find(address);
    return it != m_svcCache.end() ? it->second : std::vector<JZBluetoothUuid>();
}

void JZBleDriverSimulator::discoverCharacteristics(const std::string &address, const std::string &serviceUuid,
                                                   JZBLEService *d)
{
    { JZMutexLocker lk(&m_mutex); m_chrDiscoverCtrl = d; m_chrDiscoverAddr = address; m_chrDiscoverSvc = serviceUuid; }
    JZVariantMap m; m["addr"] = JZVariant(address); m["svc"] = JZVariant(serviceUuid);
    sendSim(SIM_CMD_DISCOVER_CHR, m);
}
std::vector<JZLowEnergyCharacteristic> JZBleDriverSimulator::characteristics(const std::string &address,
                                                                            const std::string &serviceUuid)
{
    JZMutexLocker lk(&m_mutex);
    auto it = m_charCache.find(address + "|" + serviceUuid);
    return it != m_charCache.end() ? it->second : std::vector<JZLowEnergyCharacteristic>();
}

// ===================== Central: 读写/通知 =====================
void JZBleDriverSimulator::readCharacteristic(const std::string &address, const std::string &serviceUuid,
                                              const std::string &charUuid, JZBLEService *d)
{
    int h = 0; Ctx cx;
    {
        JZMutexLocker lk(&m_mutex);
        auto it = m_charValHandle.find(address + "|" + serviceUuid);
        if (it == m_charValHandle.end()) return;
        auto it2 = it->second.find(charUuid);
        if (it2 == it->second.end()) return;
        h = it2->second;
        cx.addr = address; cx.svc = serviceUuid; cx.chr = charUuid; cx.d = d;
        m_readCtx[h] = cx;
    }
    JZVariantMap m; m["addr"] = JZVariant(address); m["svc"] = JZVariant(serviceUuid); m["chr"] = JZVariant(charUuid);
    sendSim(SIM_CMD_READ, m);
}
void JZBleDriverSimulator::writeCharacteristic(const std::string &address, const std::string &serviceUuid,
                                               const std::string &charUuid, const JZByteArray &value,
                                               bool writeNoResponse, JZBLEService *d)
{
    int h = 0; Ctx cx;
    {
        JZMutexLocker lk(&m_mutex);
        auto it = m_charValHandle.find(address + "|" + serviceUuid);
        if (it == m_charValHandle.end()) return;
        auto it2 = it->second.find(charUuid);
        if (it2 == it->second.end()) return;
        h = it2->second;
        cx.addr = address; cx.svc = serviceUuid; cx.chr = charUuid; cx.value = value; cx.d = d;
        m_writeCtx[h] = cx;
    }
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
    // m_periphChars 仅由应用线程访问（网络接收线程从不读取），无需加锁。
    PeriphChar pc; pc.svc = svcUuid; pc.chr = charUuid; pc.props = properties;
    m_periphChars.push_back(pc);
    JZVariantMap m; m["svc"] = JZVariant(svcUuid); m["chr"] = JZVariant(charUuid); m["props"] = JZVariant(properties);
    sendSim(SIM_CMD_ADD_PERIPH_CHR, m);
}
void JZBleDriverSimulator::startAdvertising(JZBLEPeripheral *d)
{
    { JZMutexLocker lk(&m_mutex); m_periphCtrl = d; }
    // name / serviceUuids 从 JZBLEPeripheral 取得（与基类签名保持一致，不含这些参数）
    std::string name = d ? d->localName() : std::string();
    std::vector<std::string> su;
    if (d) {
        for (auto &svc : d->serviceList())
            su.push_back(svc.uuid().toString());
    }
    JZVariantMap m; m["name"] = JZVariant(name); {
    JZVariantList _l; _l.reserve(su.size());
    for (auto &_e : su) _l.push_back(JZVariant(_e));
    m["su"] = JZVariant::fromValue(_l);
}
    sendSim(SIM_CMD_START_ADV, m);
}
void JZBleDriverSimulator::stopAdvertising(JZBLEPeripheral *d)
{
    (void)d;
    { JZMutexLocker lk(&m_mutex); m_periphConnHandle = -1; m_periphCtrl = nullptr; }
    JZVariantMap m;
    sendSim(SIM_CMD_STOP_ADV, m);
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

void JZBleDriverSimulator::peripheralSendNotify(JZBLEPeripheral *d, JZBLEPeripheralConnect *connect, const JZBluetoothUuid &charUuid, const JZByteArray &value)
{
    (void)d; (void)connect;
    // 模拟器不区分订阅状态，notify / indicate 都发出，由对端按需接收
    peripheralSendNotify(charUuid, value);
    peripheralSendIndicate(charUuid, value);
}

void JZBleDriverSimulator::peripheralDisconnect(JZBLEPeripheral *d, JZBLEPeripheralConnect *connect)
{
    (void)connect;
    stopAdvertising(d);
}

// ===================== 报文分发 =====================
// onRecv 在网络接收线程中执行，与公有方法（应用线程）共享全部状态，
// 故每个 case 都先加锁取出所需数据/指针，再释放锁后调用上层回调，避免重入死锁。
void JZBleDriverSimulator::onRecv(JZSimPack *sp)
{
    const JZVariantMap &map = sp->data;
    JZSimCmd cmd = sp->cmd;

    switch (cmd) {
    case SIM_EVT_SCAN_RESULT: {
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
        JZBLEDeviceInfo info;
        info.setAddress(addr);
        info.setName(name);
        info.setRssi(rssi);
        std::vector<JZBluetoothUuid> uu;
        for (auto &s : su) uu.push_back(JZBluetoothUuid(s));
        info.setServiceUuids(uu);
        JZObjectPointer agent;
        {
            JZMutexLocker lk(&m_mutex);
            agent = m_scanAgent;
        }
        if (agent)
            JZBLEDeviceDiscoveryAgent::onDeviceDiscovered((JZBLEDeviceDiscoveryAgent *)agent.object(), info);
        break;
    }
    case SIM_EVT_CONNECTED: {
        std::string addr = map.at("addr").toString();
        int role = map.at("role").toInt();
        if (role == 0) {
            JZBLEController *ctrl = nullptr;
            {
                JZMutexLocker lk(&m_mutex);
                m_connMap[addr] = 1;
                ctrl = m_central;
            }
            if (ctrl)
                JZBleDriver::centralOnDeviceConnected(ctrl, addr.c_str());
        } else {
            std::shared_ptr<JZBLEPeripheralConnect> connect;
            JZBLEPeripheral *periph = nullptr;
            {
                JZMutexLocker lk(&m_mutex);
                m_periphConnHandle = 1;
                connect = std::make_shared<JZBLEPeripheralConnect>();
                m_periphConnect = connect;
                periph = m_periphCtrl;
            }
            if (periph) JZBleDriver::peripheralOnClientConnected(periph, connect);
        }
        break;
    }
    case SIM_EVT_DISCONNECTED: {
        std::string addr = map.at("addr").toString();
        int role = map.at("role").toInt();
        if (role == 0) {
            JZBLEController *ctrl = nullptr;
            {
                JZMutexLocker lk(&m_mutex);
                m_connMap.erase(addr);
                ctrl = m_central;
            }
            if (ctrl)
                JZBleDriver::centralOnDeviceDisconnected(ctrl, addr.c_str());
        } else {
            JZBLEPeripheral *periph = nullptr;
            std::shared_ptr<JZBLEPeripheralConnect> connect;
            {
                JZMutexLocker lk(&m_mutex);
                m_periphConnHandle = -1;
                periph = m_periphCtrl;
                connect = m_periphConnect;
                m_periphConnect.reset();
            }
            if (periph) JZBleDriver::peripheralOnClientDisconnected(periph, connect);
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
        JZBLEController *ctrl = nullptr;
        {
            JZMutexLocker lk(&m_mutex);
            m_svcCache[addr] = vu;
            ctrl = m_svcDiscoverCtrl;
        }
        if (ctrl)
            JZBleDriver::centralOnServicesDiscovered(ctrl, addr.c_str());
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
        JZBLEService *ctrl = nullptr;
        {
            JZMutexLocker lk(&m_mutex);
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
            ctrl = m_chrDiscoverCtrl;
        }
        if (ctrl)
            JZBLEService::onCharacteristicsDiscovered(ctrl, addr.c_str(), svc.c_str());
        break;
    }
    case SIM_EVT_READ_RESULT: {
        std::string addr = map.at("addr").toString();
        std::string svc  = map.at("svc").toString();
        std::string chr  = map.at("chr").toString();
        std::string valStr = map.at("val").toString();
        JZByteArray val(valStr.data(), (int)valStr.size());
        JZBLEService *d = nullptr;
        {
            JZMutexLocker lk(&m_mutex);
            int h = 0;
            auto it = m_charValHandle.find(addr + "|" + svc);
            if (it != m_charValHandle.end()) {
                auto it2 = it->second.find(chr);
                if (it2 != it->second.end()) h = it2->second;
            }
            auto cit = m_readCtx.find(h);
            if (cit != m_readCtx.end()) {
                d = cit->second.d;
                m_readCtx.erase(cit);
            }
        }
        if (d) JZBLEService::onCharacteristicRead(d, addr.c_str(), svc.c_str(), val);
        break;
    }
    case SIM_EVT_WRITE_RESULT: {
        std::string addr = map.at("addr").toString();
        std::string svc  = map.at("svc").toString();
        std::string chr  = map.at("chr").toString();
        JZByteArray val;
        JZBLEService *d = nullptr;
        {
            JZMutexLocker lk(&m_mutex);
            int h = 0;
            auto it = m_charValHandle.find(addr + "|" + svc);
            if (it != m_charValHandle.end()) {
                auto it2 = it->second.find(chr);
                if (it2 != it->second.end()) h = it2->second;
            }
            auto cit = m_writeCtx.find(h);
            if (cit != m_writeCtx.end()) {
                d = cit->second.d;
                val = cit->second.value;
                m_writeCtx.erase(cit);
            }
        }
        if (d) JZBLEService::onCharacteristicWritten(d, addr.c_str(), svc.c_str(), val, true);
        break;
    }
    case SIM_EVT_NOTIFY_RX: {
        std::string addr = map.at("addr").toString();
        std::string svc  = map.at("svc").toString();
        std::string chr  = map.at("chr").toString();
        std::string valStr = map.at("val").toString();
        JZByteArray val(valStr.data(), (int)valStr.size());
        JZBLEService *d = nullptr;
        {
            JZMutexLocker lk(&m_mutex);
            int h = 0;
            auto it = m_charValHandle.find(addr + "|" + svc);
            if (it != m_charValHandle.end()) {
                auto it2 = it->second.find(chr);
                if (it2 != it->second.end()) h = it2->second;
            }
            auto cit = m_ntfCtx.find(h);
            if (cit != m_ntfCtx.end())
                d = cit->second.d;
        }
        if (d) JZBLEService::onCharacteristicChanged(d, addr.c_str(), svc.c_str(), val);
        break;
    }
    case SIM_EVT_CHAR_WRITTEN: {
        std::string chr = map.at("chr").toString();
        std::string val = map.at("val").toString();
        JZByteArray bval(val.data(), (int)val.size());
        JZBLEPeripheral *periph = nullptr;
        std::shared_ptr<JZBLEPeripheralConnect> connect;
        {
            JZMutexLocker lk(&m_mutex);
            periph = m_periphCtrl;
            connect = m_periphConnect;
        }
        if (periph) JZBleDriver::peripheralOnCharWritten(periph, connect, JZBluetoothUuid(chr), bval);
        break;
    }
    default:
        break;
    }
}
