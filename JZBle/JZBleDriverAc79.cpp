#include "JZBleDriverAc79.h"
#include "JZBLEService.h"
#include "JZBLECentral.h"
#include "JZBLEPeripheral.h"
#include "JZBLEDeviceDiscoveryAgent.h"
#include "JZStd.h"

#include <algorithm>
#include <cstring>
#include <cstdlib>

// ===================== AC79 BLE 协议栈头文件 =====================
// 这些头文件来自杰理 BLE SDK (include_lib/btstack)，仅在对应固件工程中可见。
// 本文件由固件 CMake 直接编译，并链接对应平台的 btstack.a 与 btctrler.a
// (路径随平台而变, 如 AC79 为 cpu/wl82/liba, BR28 为 cpu/br28/liba)。
extern "C" {
#include "btstack/bluetooth.h"
#include "btstack/btstack_event.h"
#include "btstack/le/ble_api.h"
#include "btstack/le/le_user.h"
#include "btstack/le/att.h"
#include "btstack/le/gatt.h"
}

// ATT 属性标志(与 att.h 中 ATT_PROPERTY_* 一致)
static const int AC79_ATT_READ     = 0x02;
static const int AC79_ATT_WRITE_NR = 0x04;
static const int AC79_ATT_WRITE    = 0x08;
static const int AC79_ATT_NOTIFY   = 0x10;
static const int AC79_ATT_INDICATE = 0x20;

// 本地 ATT 相关缓存参数
#define AC79_LOCAL_MTU       200
static uint8_t s_attRam[1024]   __attribute__((aligned(4)));
static uint8_t s_searchBuf[512] __attribute__((aligned(4)));

// ===================== 工具 =====================
std::string JZBleDriverAc79::addrToString(const uint8_t *addr)
{
    char buf[18];
    snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
    return std::string(buf);
}
void JZBleDriverAc79::addrFromString(const std::string &str, uint8_t *addrOut)
{
    for (int i = 0, pos = 0; i < 6; i++, pos += 3) {
        uint8_t hi = (uint8_t)(str[pos] > '9' ? str[pos] - 'a' + 10 : str[pos] - '0');
        uint8_t lo = (uint8_t)(str[pos + 1] > '9' ? str[pos + 1] - 'a' + 10 : str[pos + 1] - '0');
        addrOut[5 - i] = (hi << 4) | lo;
    }
}

int JZBleDriverAc79::jzPropToAtt(int p)
{
    int a = 0;
    if (p & 0x01) a |= AC79_ATT_READ;
    if (p & 0x02) a |= AC79_ATT_WRITE;
    if (p & 0x04) a |= AC79_ATT_NOTIFY;
    if (p & 0x08) a |= AC79_ATT_INDICATE;
    if (p & 0x10) a |= AC79_ATT_WRITE_NR;
    return a;
}
int JZBleDriverAc79::attPropToJz(int p)
{
    int a = 0;
    if (p & AC79_ATT_READ)     a |= 0x01;
    if (p & AC79_ATT_WRITE)    a |= 0x02;
    if (p & AC79_ATT_NOTIFY)   a |= 0x04;
    if (p & AC79_ATT_INDICATE) a |= 0x08;
    if (p & AC79_ATT_WRITE_NR) a |= 0x10;
    return a;
}

void JZBleDriverAc79::uuidToBytes(const JZBluetoothUuid &u, std::vector<uint8_t> &out, bool &is16)
{
    uint16_t v16 = u.toUInt16();
    if (v16 != 0) {
        is16 = true;
        out.push_back((uint8_t)(v16 & 0xff));
        out.push_back((uint8_t)(v16 >> 8));
        return;
    }
    is16 = false;
    std::string s = u.toString();          // xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
    std::string hex;
    for (char c : s) if (c != '-') hex.push_back(c);
    if (hex.size() != 32) return;
    uint8_t bytes[16];
    for (int i = 0; i < 16; i++)
        bytes[i] = (uint8_t)strtol(hex.substr(i * 2, 2).c_str(), nullptr, 16);
    // profile_data 中 128-bit UUID 以小端存储, 故反转
    for (int i = 15; i >= 0; i--) out.push_back(bytes[i]);
}

static void pushEntry(std::vector<uint8_t> &db, uint16_t handle, uint16_t flags,
                      const std::vector<uint8_t> &uuid, const std::vector<uint8_t> &value)
{
    uint16_t size = (uint16_t)(2 + 2 + 2 + uuid.size() + value.size());
    db.push_back((uint8_t)(size & 0xff));
    db.push_back((uint8_t)((size >> 8) & 0xff));
    db.push_back((uint8_t)(flags & 0xff));
    db.push_back((uint8_t)((flags >> 8) & 0xff));
    db.push_back((uint8_t)(handle & 0xff));
    db.push_back((uint8_t)((handle >> 8) & 0xff));
    for (uint8_t b : uuid) db.push_back(b);
    for (uint8_t b : value) db.push_back(b);
}

static bool contains(const std::vector<JZBluetoothUuid> &v, const JZBluetoothUuid &u)
{
    return std::find(v.begin(), v.end(), u) != v.end();
}

static JZBluetoothUuid uuidFromResult(bool is16, uint16_t u16, const uint8_t u128[16])
{
    if (is16) return JZBluetoothUuid(u16);
    static const char *hx = "0123456789abcdef";
    std::string s;
    auto app = [&](int i) { s.push_back(hx[u128[i] >> 4]); s.push_back(hx[u128[i] & 0xf]); };
    for (int i = 0; i < 4; i++) app(i); s.push_back('-');
    for (int i = 4; i < 6; i++) app(i);  s.push_back('-');
    for (int i = 6; i < 8; i++) app(i);  s.push_back('-');
    for (int i = 8; i < 10; i++) app(i); s.push_back('-');
    for (int i = 10; i < 16; i++) app(i);
    return JZBluetoothUuid(s);
}

// ===================== 构造/析构 =====================
JZBleDriverAc79::JZBleDriverAc79()
{
    // 注意: 固件必须先在 ble_stack_init()/bt_task 启动后再实例化本驱动,
    // 否则以下协议栈初始化接口可能失败。
    gatt_client_init();
    le_device_db_init();
    gatt_client_register_packet_handler(ac79_packet_handler);
    att_server_register_packet_handler(ac79_packet_handler);
    hci_event_callback_set(ac79_packet_handler);
    le_l2cap_register_packet_handler(ac79_packet_handler);
}
JZBleDriverAc79::~JZBleDriverAc79() {}
bool JZBleDriverAc79::isBluetoothAvailable() { return true; }
bool JZBleDriverAc79::isBluetoothEnabled()   { return true; }

// ===================== Central: 扫描 =====================
void JZBleDriverAc79::startScan()
{
    if (m_scanning) return;
    m_scanning = true;
    // SCAN_ACTIVE, interval=24(0.625ms), window=8(0.625ms)
    ble_op_set_scan_param(0x01, 24, 8);
    ble_op_scan_enable2(1, 0);
}
void JZBleDriverAc79::stopScan()
{
    m_scanning = false;
    ble_op_scan_enable2(0, 0);
}
bool JZBleDriverAc79::isScanning() { return m_scanning; }

void JZBleDriverAc79::parseAdvName(const uint8_t *data, uint8_t len, std::string &name)
{
    uint8_t pos = 0;
    while (pos + 1 < len) {
        uint8_t l = data[pos];
        if (l == 0) break;
        uint8_t type = data[pos + 1];
        if (type == 0x08 || type == 0x09) {           // 缩写/完整本地名
            name.assign((const char *)&data[pos + 2], l - 1);
            return;
        }
        pos += l + 1;
    }
}

// ===================== Central: 连接 =====================
void JZBleDriverAc79::connectToDevice(JZBLECentral *d)
{
    std::string addr = d->deviceInfo().address();
    m_centralByAddr[addr] = d;
    // 协议栈要求连接参数指针在连接期间保持有效, 故用静态变量
    static struct create_conn_param_t s_cp;
    memset(&s_cp, 0, sizeof(s_cp));
    s_cp.conn_interval        = 24;    // unit:1.25ms
    s_cp.conn_latency         = 0;
    s_cp.supervision_timeout  = 400;   // unit:10ms
    s_cp.peer_address_type    = 0;     // public
    addrFromString(addr, s_cp.peer_address);
    ble_op_create_connection(&s_cp);
}
void JZBleDriverAc79::disconnectFromDevice(JZBLECentral *d)
{
    int h = getConnHandle(d->deviceInfo().address());
    if (h >= 0) ble_op_disconnect(h);
}
bool JZBleDriverAc79::isConnected(JZBLECentral *d) { return getConnHandle(d->deviceInfo().address()) >= 0; }

int JZBleDriverAc79::getConnHandle(const std::string &a)
{
    auto it = m_conHandleByAddr.find(a);
    return it != m_conHandleByAddr.end() ? it->second : -1;
}

// ===================== Central: 服务/特征发现 =====================
void JZBleDriverAc79::discoverServices(const std::string &address, JZBLECentral *d)
{
    m_svcDiscoverCtrl = d;
    m_svcDiscoverAddr = address;
    int h = getConnHandle(address);
    if (h < 0) return;
    user_client_init((uint16_t)h, s_searchBuf, (uint16_t)sizeof(s_searchBuf));
    ble_op_search_profile_all();
}
std::vector<JZBluetoothUuid> JZBleDriverAc79::services(const std::string &address)
{
    auto it = m_svcCache.find(address);
    return it != m_svcCache.end() ? it->second : std::vector<JZBluetoothUuid>();
}

void JZBleDriverAc79::discoverCharacteristics(const std::string &address, const std::string &serviceUuid,
                                              JZBLEServicePrivate *d)
{
    std::string key = address + "|" + serviceUuid;
    auto it = m_charCache.find(key);
    if (it != m_charCache.end()) {
        for (auto &c : it->second) {
            auto vit = m_charValHandle[key].find(c.uuid().toString());
            if (vit != m_charValHandle[key].end())
                m_ntfCtx[vit->second] = {address, serviceUuid, d};
        }
    }
    JZBLEServicePrivate::onCharacteristicsDiscovered(d, address.c_str(), serviceUuid.c_str());
}
std::vector<JZLowEnergyCharacteristic> JZBleDriverAc79::characteristics(const std::string &address,
                                                                        const std::string &serviceUuid)
{
    auto it = m_charCache.find(address + "|" + serviceUuid);
    return it != m_charCache.end() ? it->second : std::vector<JZLowEnergyCharacteristic>();
}

// ===================== Central: 读写/通知 =====================
void JZBleDriverAc79::readCharacteristic(const std::string &address, const std::string &serviceUuid,
                                         const std::string &charUuid, JZBLEServicePrivate *d)
{
    std::string key = address + "|" + serviceUuid;
    auto vit = m_charValHandle[key].find(charUuid);
    if (vit == m_charValHandle[key].end()) return;
    uint16_t vh = vit->second;
    m_readCtx[vh] = {address, serviceUuid, charUuid, d};
    uint16_t dummy = 0;
    ble_op_att_send_data(vh, (uint8_t *)&dummy, 2, ATT_OP_READ);
}
void JZBleDriverAc79::writeCharacteristic(const std::string &address, const std::string &serviceUuid,
                                          const std::string &charUuid, const std::string &value,
                                          bool writeNoResponse, JZBLEServicePrivate *d)
{
    std::string key = address + "|" + serviceUuid;
    auto vit = m_charValHandle[key].find(charUuid);
    if (vit == m_charValHandle[key].end()) return;
    uint8_t op = writeNoResponse ? ATT_OP_WRITE_WITHOUT_RESPOND : ATT_OP_WRITE;
    ble_op_att_send_data(vit->second, (uint8_t *)value.data(), (uint16_t)value.size(), op);
    // 尽力反馈写入结果
    JZBLEServicePrivate::onCharacteristicWritten(d, address.c_str(), serviceUuid.c_str(), value.c_str());
}
void JZBleDriverAc79::enableNotification(const std::string &address, const std::string &serviceUuid,
                                         const std::string &charUuid, bool enable, JZBLEServicePrivate *d)
{
    std::string key = address + "|" + serviceUuid;
    auto vit = m_charValHandle[key].find(charUuid);
    if (vit == m_charValHandle[key].end()) return;
    uint16_t ccc = vit->second + 1;          // CCC handle = value handle + 1
    uint16_t v = enable ? 0x0001 : 0x0000;   // 小端
    ble_op_att_send_data(ccc, (uint8_t *)&v, 2, ATT_OP_WRITE);
}
void JZBleDriverAc79::readDescriptor(const std::string &address, const std::string &serviceUuid,
                                     const std::string &charUuid, const std::string &descUuid,
                                     JZBLEServicePrivate *d)
{
    JZBLEServicePrivate::onDescriptorRead(d, address.c_str(), serviceUuid.c_str(), descUuid.c_str(), "");
}
void JZBleDriverAc79::writeDescriptor(const std::string &address, const std::string &serviceUuid,
                                      const std::string &charUuid, const std::string &descUuid,
                                      const std::string &value, JZBLEServicePrivate *d)
{
    JZBLEServicePrivate::onDescriptorWritten(d, address.c_str(), serviceUuid.c_str(), descUuid.c_str(), value.c_str());
}

// ===================== Peripheral: 构建 GATT 数据库 =====================
std::vector<uint8_t> JZBleDriverAc79::buildProfileData()
{
    std::vector<uint8_t> db;
    uint16_t h = 1;

    // 收集唯一服务(保持注册顺序)
    std::vector<std::string> svcs;
    for (auto &pc : m_periphChars)
        if (!contains(svcs, JZBluetoothUuid(pc.svc))) svcs.push_back(pc.svc);

    for (auto &svc : svcs) {
        std::vector<PeriphChar> chars;
        for (auto &pc : m_periphChars)
            if (pc.svc == svc) chars.push_back(pc);

        // PRIMARY_SERVICE 声明
        std::vector<uint8_t> svcBytes; bool s16;
        uuidToBytes(JZBluetoothUuid(svc), svcBytes, s16);
        pushEntry(db, h++, 0x0002, {0x00, 0x28}, svcBytes);

        for (auto &c : chars) {
            std::vector<uint8_t> cBytes; bool c16;
            uuidToBytes(JZBluetoothUuid(c.chr), cBytes, c16);
            int attProp   = jzPropToAtt(c.properties);
            uint16_t decl = h++;
            uint16_t val  = h++;

            // CHARACTERISTIC 声明: [prop(1), valueHandle(2 LE), charUuid]
            std::vector<uint8_t> declVal;
            declVal.push_back((uint8_t)attProp);
            declVal.push_back((uint8_t)(val & 0xff));
            declVal.push_back((uint8_t)((val >> 8) & 0xff));
            for (uint8_t b : cBytes) declVal.push_back(b);
            pushEntry(db, decl, 0x0002, {0x03, 0x28}, declVal);

            // CHARACTERISTIC VALUE
            int perm     = (attProp & (AC79_ATT_READ | AC79_ATT_WRITE)) ? 0x01 : 0x00;
            int valFlags = (perm << 8) | attProp;
            pushEntry(db, val, (uint16_t)valFlags, cBytes, {});

            m_periphValHandle[c.chr]  = val;
            m_periphHandleChar[val]   = c.chr;
            m_periphValueStore[val]    = "";

            // 需要通知/指示时追加 CCCD
            if (attProp & (AC79_ATT_NOTIFY | AC79_ATT_INDICATE)) {
                uint16_t ccc = h++;
                pushEntry(db, ccc, 0x010a, {0x02, 0x29}, {0x00, 0x00});
                m_periphCcc[ccc]          = true;
                m_periphHandleChar[ccc]   = c.chr;
            }
        }
    }
    db.push_back(0); db.push_back(0);   // 结束符
    return db;
}

void JZBleDriverAc79::addPeripheralCharacteristic(const std::string &svcUuid, const std::string &charUuid, int properties)
{
    m_periphChars.push_back({svcUuid, charUuid, properties});
}

void JZBleDriverAc79::startAdvertising(const std::string &name, const std::vector<JZBluetoothUuid> &serviceUuids,
                                       JZBLEPeripheral *d)
{
    m_periphCtrl     = d;
    m_advertising    = true;
    m_periphConnHandle = -1;

    // 构建并注册 GATT 数据库
    std::vector<uint8_t> db = buildProfileData();
    att_server_init(db.data(), ac79_att_read_cb, ac79_att_write_cb);
    att_server_register_packet_handler(ac79_packet_handler);

    // 广播数据: Flags + 16-bit 服务 UUID 列表
    std::vector<uint8_t> adv;
    adv.push_back(0x02); adv.push_back(0x01); adv.push_back(0x06);   // LE General Discoverable
    std::vector<uint8_t> svcList;
    for (auto &u : serviceUuids) {
        uint16_t v = u.toUInt16();
        if (v != 0) { svcList.push_back((uint8_t)(v & 0xff)); svcList.push_back((uint8_t)(v >> 8)); }
    }
    if (!svcList.empty()) {
        adv.push_back((uint8_t)(svcList.size() + 1));
        adv.push_back(0x03);   // 完整 16-bit 服务 UUID 列表
        for (uint8_t b : svcList) adv.push_back(b);
    }
    ble_op_set_adv_param(0x0200, 0x00 /*ADV_IND*/, 0x07 /*ADV_CHANNEL_ALL*/);
    ble_op_set_adv_data((int)adv.size(), adv.data());

    // 扫描响应: 完整本地名
    std::vector<uint8_t> rsp;
    uint8_t nl = (uint8_t)name.size();
    if (nl > 28) nl = 28;
    rsp.push_back((uint8_t)(nl + 1));
    rsp.push_back(0x09);   // Complete Local Name
    for (uint8_t i = 0; i < nl; i++) rsp.push_back((uint8_t)name[i]);
    ble_op_set_rsp_data((int)rsp.size(), rsp.data());

    ble_op_adv_enable(1);
}
void JZBleDriverAc79::stopAdvertising()
{
    ble_op_adv_enable(0);
    m_advertising      = false;
    m_periphConnHandle = -1;
    m_periphCtrl       = nullptr;
}
void JZBleDriverAc79::peripheralSendNotify(const std::string &charUuid, const std::string &value)
{
    if (m_periphConnHandle < 0) return;
    auto it = m_periphValHandle.find(charUuid);
    if (it == m_periphValHandle.end()) return;
    uint16_t valHandle = (uint16_t)it->second;
    // 官方示例: 先查 CCCD 使能(=valueHandle+1), 未使能则不发
    if (!att_get_ccc_config(valHandle + 1)) return;
    ble_op_att_send_data(valHandle, (uint8_t *)value.data(), (uint16_t)value.size(),
                         ATT_OP_AUTO_READ_CCC);
}
void JZBleDriverAc79::peripheralSendIndicate(const std::string &charUuid, const std::string &value)
{
    if (m_periphConnHandle < 0) return;
    auto it = m_periphValHandle.find(charUuid);
    if (it == m_periphValHandle.end()) return;
    uint16_t valHandle = (uint16_t)it->second;
    // 官方示例: 先查 CCCD 使能(=valueHandle+1), 未使能则不发
    if (!att_get_ccc_config(valHandle + 1)) return;
    ble_op_att_send_data(valHandle, (uint8_t *)value.data(), (uint16_t)value.size(),
                         ATT_OP_AUTO_READ_CCC);
}

// ===================== 报文分发 =====================
void JZBleDriverAc79::onPacket(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    (void)channel; (void)size;
    switch (packet_type) {
    case HCI_EVENT_PACKET:
        switch (hci_event_packet_get_type(packet)) {
        case HCI_EVENT_LE_META:
            switch (hci_event_le_meta_get_subevent_code(packet)) {
            case HCI_SUBEVENT_LE_CONNECTION_COMPLETE:
            case HCI_SUBEVENT_LE_ENHANCED_CONNECTION_COMPLETE: {
                uint8_t role = packet[6];   // 0=master(central), 1=slave(peripheral)
                uint16_t handle = hci_subevent_le_connection_complete_get_connection_handle(packet);
                uint8_t addrBytes[6];
                memcpy(addrBytes, &packet[8], 6);
                std::string addr = addrToString(addrBytes);
                if (role == 0x01) {                 // 从机角色
                    m_periphConnHandle = handle;
                    ble_op_att_send_init(handle, s_attRam, (uint16_t)sizeof(s_attRam), AC79_LOCAL_MTU);
                    JZBleDriver::peripheralOnClientConnected(m_periphCtrl);
                } else {                             // 主机角色
                    m_conHandleByAddr[addr] = handle;
                    m_addrByConHandle[handle] = addr;
                    auto it = m_centralByAddr.find(addr);
                    if (it != m_centralByAddr.end())
                        JZBleDriver::centralOnDeviceConnected(it->second, addr.c_str());
                }
                break;
            }
            default:
                break;
            }
            break;

        case HCI_EVENT_DISCONNECTION_COMPLETE: {
            uint16_t handle = little_endian_read_16(packet, 3);
            if (m_periphConnHandle == (int)handle) {
                m_periphConnHandle = -1;
                JZBleDriver::peripheralOnClientDisconnected(m_periphCtrl);
            } else {
                auto it = m_addrByConHandle.find(handle);
                if (it != m_addrByConHandle.end()) {
                    std::string addr = it->second;
                    m_conHandleByAddr.erase(addr);
                    m_addrByConHandle.erase(handle);
                    auto cit = m_centralByAddr.find(addr);
                    if (cit != m_centralByAddr.end())
                        JZBleDriver::centralOnDeviceDisconnected(cit->second, addr.c_str());
                }
            }
            break;
        }

        case ATT_EVENT_MTU_EXCHANGE_COMPLETE: {
            int mtu = att_event_mtu_exchange_complete_get_MTU(packet) - 3;
            ble_op_att_set_send_mtu((int)mtu);
            break;
        }

        case GAP_EVENT_ADVERTISING_REPORT: {
            adv_report_t *r = (adv_report_t *)&packet[2];
            JZBluetoothDeviceInfo info;
            info.setAddress(addrToString(r->address));
            info.setRssi(r->rssi);
            std::string name;
            parseAdvName(r->data, r->length, name);
            if (!name.empty()) info.setName(name);
            for (auto *a : m_agents)
                JZBLEDeviceDiscoveryAgentPrivate::onDeviceDiscovered(a, info);
            break;
        }
        default:
            break;
        }
        break;
    }
}

// ===================== 全局 C 回调(协议栈直接调用) =====================
extern "C" void ac79_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    auto *self = static_cast<JZBleDriverAc79 *>(JZBleDriver::instance());
    if (self) self->onPacket(packet_type, channel, packet, size);
}

// 由预编译 btstack.a 按固定 C 符号名直接调用, 函数名不可改
extern "C" void user_client_report_search_result(void *result_info)
{
    auto *self = static_cast<JZBleDriverAc79 *>(JZBleDriver::instance());
    if (!self) return;
    search_result_t *r = (search_result_t *)result_info;
    if (r == (search_result_t *)-1) {           // 搜索完成
        if (self->m_svcDiscoverCtrl)
            JZBleDriver::centralOnServicesDiscovered(self->m_svcDiscoverCtrl, self->m_svcDiscoverAddr.c_str());
        return;
    }
    std::string addr = self->m_svcDiscoverAddr;
    JZBluetoothUuid svc = uuidFromResult(r->services.uuid16 != 0, r->services.uuid16, r->services.uuid128);
    JZBluetoothUuid chr = uuidFromResult(r->characteristic.uuid16 != 0, r->characteristic.uuid16, r->characteristic.uuid128);

    auto &svcVec = self->m_svcCache[addr];
    if (!contains(svcVec, svc)) svcVec.push_back(svc);

    std::string key = addr + "|" + svc.toString();
    JZLowEnergyCharacteristic c;
    c.setUuid(chr);
    c.setProperties(self->attPropToJz(r->characteristic.properties));
    self->m_charCache[key].push_back(c);
    self->m_charValHandle[key][chr.toString()] = r->characteristic.value_handle;
}

extern "C" void user_client_report_data_callback(void *report_data)
{
    auto *self = static_cast<JZBleDriverAc79 *>(JZBleDriver::instance());
    if (!self) return;
    att_data_report_t *r = (att_data_report_t *)report_data;
    std::string val((const char *)r->blob, r->blob_length);
    if (r->packet_type == GATT_EVENT_CHARACTERISTIC_VALUE_QUERY_RESULT) {
        auto it = self->m_readCtx.find(r->value_handle);
        if (it != self->m_readCtx.end()) {
            ReadCtx c = it->second;
            JZBLEServicePrivate::onCharacteristicRead(c.d, c.addr.c_str(), c.svc.c_str(), val.c_str());
            self->m_readCtx.erase(it);
        }
    } else if (r->packet_type == GATT_EVENT_NOTIFICATION || r->packet_type == GATT_EVENT_INDICATION) {
        auto it = self->m_ntfCtx.find(r->value_handle);
        if (it != self->m_ntfCtx.end()) {
            NtfCtx c = it->second;
            JZBLEServicePrivate::onCharacteristicChanged(c.d, c.addr.c_str(), c.svc.c_str(), val.c_str());
        }
    }
}

extern "C" uint16_t ac79_att_read_cb(uint16_t con_handle, uint16_t att_handle, uint16_t offset,
                                     uint8_t *buffer, uint16_t buffer_size)
{
    (void)con_handle;
    auto *self = static_cast<JZBleDriverAc79 *>(JZBleDriver::instance());
    if (!self) return 0;
    auto it = self->m_periphValueStore.find(att_handle);
    if (it == self->m_periphValueStore.end()) {
        // CCCD 读: 返回已保存的使能状态(官方示例 att_read_callback 同名逻辑)
        if (self->m_periphCcc.count(att_handle)) {
            if (buffer) {
                uint16_t cfg = att_get_ccc_config(att_handle);
                buffer[0] = (uint8_t)(cfg & 0xff);
                buffer[1] = (uint8_t)((cfg >> 8) & 0xff);
            }
            return 2;
        }
        return 0;
    }
    const std::string &v = it->second;
    if (!buffer) return (uint16_t)v.size();
    if (offset >= v.size()) return 0;
    uint16_t n = (uint16_t)std::min((size_t)(v.size() - offset), (size_t)buffer_size);
    memcpy(buffer, v.data() + offset, n);
    return n;
}

extern "C" int ac79_att_write_cb(uint16_t con_handle, uint16_t att_handle, uint16_t transaction_mode,
                                 uint16_t offset, uint8_t *buffer, uint16_t buffer_size)
{
    (void)con_handle; (void)transaction_mode; (void)offset;
    auto *self = static_cast<JZBleDriverAc79 *>(JZBleDriver::instance());
    if (!self) return 0;
    if (self->m_periphCcc.count(att_handle)) {
        // CCCD 写: 维护使能状态(官方示例 att_write_callback 同名逻辑)
        att_set_ccc_config(att_handle, buffer ? buffer[0] : 0);
        return 0;
    }
    auto it = self->m_periphHandleChar.find(att_handle);
    if (it == self->m_periphHandleChar.end()) return 0;
    std::string val((const char *)buffer, buffer_size);
    self->m_periphValueStore[att_handle] = val;
    JZBleDriver::peripheralOnCharWritten(self->m_periphCtrl, it->second.c_str(), val.c_str());
    return 0;
}
