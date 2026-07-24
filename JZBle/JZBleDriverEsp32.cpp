#include "JZBleDriverEsp32.h"
#include "JZBLEService.h"
#include "JZBLEController.h"
#include "JZBLEPeripheral.h"
#include "JZBLEDeviceDiscoveryAgent.h"
#include "JZStd.h"
#include "JZByteArray.h"

#include "esp_bt.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/ble_store.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/util/util.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include <cstdio>
#include <cinttypes>
#include <cstring>

// 前向声明
static void gatt_svr_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg);
extern "C" void ble_gatts_show_local(void);

static JZBleDriverEsp32 *g_driver = nullptr;

// Peripheral GATT 定义（必须长期存活，不能为局部变量）
struct PeriphGatt {
    std::vector<ble_uuid_any_t>   svcUuids;
    std::vector<ble_uuid_any_t>   chrUuids;
    std::vector<ble_gatt_chr_def> chrDefs;
    std::vector<ble_gatt_svc_def> svcDefs;
};

// ==================== 工具 ====================
std::string JZBleDriverEsp32::addrToString(const uint8_t *addr)
{
    char buf[18]; snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x",
        addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
    return std::string(buf);
}
void JZBleDriverEsp32::addrFromString(const std::string &str, uint8_t *addrOut)
{
    // 期望格式 "xx:xx:xx:xx:xx:xx"（小写）
    if (str.size() < 17) return;
    for (int i = 0, pos = 0; i < 6; i++, pos += 3) {
        char hi = str[pos], lo = str[pos + 1];
        uint8_t h = (uint8_t)(hi > '9' ? hi - 'a' + 10 : hi - '0');
        uint8_t l = (uint8_t)(lo > '9' ? lo - 'a' + 10 : lo - '0');
        addrOut[5 - i] = (h << 4) | l;
    }
}
int JZBleDriverEsp32::uuidFromString(const std::string &str, void *uuidOut)
{
    ble_uuid_any_t *u = (ble_uuid_any_t *)uuidOut;
    // 纯短格式（无 '-'）：4 位=16-bit，8 位=32-bit
    if (str.find('-') == std::string::npos) {
        if (str.size() == 4) {
            u->u.type = BLE_UUID_TYPE_16;
            u->u16.value = (uint16_t)strtol(str.c_str(), nullptr, 16);
            return 0;
        }
        if (str.size() == 8) {
            u->u.type = BLE_UUID_TYPE_32;
            u->u32.value = (uint32_t)strtoul(str.c_str(), nullptr, 16);
            return 0;
        }
    }
    // 128-bit 完整格式（含 Base UUID 后缀）
    if (str_startsWith(str, "0000") && str_endsWith(str, "-0000-1000-8000-00805f9b34fb"))
        { u->u.type = BLE_UUID_TYPE_16; u->u16.value = (uint16_t)strtol(str.substr(4, 4).c_str(), nullptr, 16); return 0; }
    if (str_endsWith(str, "-0000-1000-8000-00805f9b34fb"))
        { u->u.type = BLE_UUID_TYPE_32; u->u32.value = (uint32_t)strtoul(str.substr(0, 8).c_str(), nullptr, 16); return 0; }
    u->u.type = BLE_UUID_TYPE_128;
    return ble_uuid_from_str(u, str.c_str()) == 0 ? 0 : -1;
}

// ble_uuid_t -> JZBluetoothUuid（统一为 128-bit 标准字符串）
JZBluetoothUuid JZBleDriverEsp32::bleUuidToJZ(const ble_uuid_t *u)
{
    char buf[40];
    if (u->type == BLE_UUID_TYPE_16) {
        uint16_t v = ((const ble_uuid16_t *)u)->value;
        snprintf(buf, sizeof(buf), "0000%04x-0000-1000-8000-00805f9b34fb", v);
    } else if (u->type == BLE_UUID_TYPE_32) {
        uint32_t v = ((const ble_uuid32_t *)u)->value;
        snprintf(buf, sizeof(buf), "%08" PRIx32 "-0000-1000-8000-00805f9b34fb", v);
    } else {
        const uint8_t *b = ((const ble_uuid128_t *)u)->value;
        snprintf(buf, sizeof(buf),
            "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
            b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    }
    return JZBluetoothUuid(buf);
}

bool JZBleDriverEsp32::uuidEqual(const JZBluetoothUuid &a, const std::string &b)
{
    // 把 b 规范化成 128-bit 小写形式（uuidFromString 已支持短格式），再与 a 比较
    ble_uuid_any_t u;
    if (uuidFromString(b, &u) != 0) return a.toString() == b;
    return a.toString() == bleUuidToJZ(&u.u).toString();
}

std::string JZBleDriverEsp32::normalizeUuid(const std::string &s)
{
    ble_uuid_any_t u;
    if (uuidFromString(s, &u) != 0) return s;
    return bleUuidToJZ(&u.u).toString();
}

int  JZBleDriverEsp32::getConnHandle(const std::string &a) { auto it = m_connMap.find(a); return it != m_connMap.end() ? it->second : -1; }
void JZBleDriverEsp32::setConnHandle(const std::string &a, int h) { m_connMap[a] = h; m_connToAddr[h] = a; }
void JZBleDriverEsp32::removeConnHandle(const std::string &a)
{
    auto it = m_connMap.find(a);
    if (it != m_connMap.end()) { m_connToAddr.erase(it->second); m_connMap.erase(it); }
}
std::string JZBleDriverEsp32::connHandleToAddr(int h) const
{
    auto it = m_connToAddr.find(h);
    return it != m_connToAddr.end() ? it->second : "";
}

uint16_t JZBleDriverEsp32::findCharValHandle(const std::string &addr, const std::string &svc, const std::string &c)
{
    auto it = m_gatt.find(addr);
    if (it == m_gatt.end()) return 0;
    auto cit = it->second.chars.find(svc);
    if (cit == it->second.chars.end()) return 0;
    for (auto &ch : cit->second) if (uuidEqual(ch.uuid, c)) return ch.valHandle;
    return 0;
}
uint16_t JZBleDriverEsp32::findDscHandle(const std::string &addr, const std::string &svc, const std::string &c, const std::string &desc)
{
    auto it = m_gatt.find(addr);
    if (it == m_gatt.end()) return 0;
    auto dit = it->second.dscs.find(svc + "|" + normalizeUuid(c));
    if (dit == it->second.dscs.end()) return 0;
    for (auto &ds : dit->second) if (uuidEqual(ds.uuid, desc)) return ds.handle;
    return 0;
}

void JZBleDriverEsp32::writeCccd(int connHandle, uint16_t cccdHandle, bool enable)
{
    uint8_t val[2] = { (uint8_t)(enable ? 0x01 : 0x00), 0x00 };
    ble_gattc_write_flat(connHandle, cccdHandle, val, sizeof(val), onGattcAck, nullptr);
}

void JZBleDriverEsp32::ensureDescriptorsDiscovered(const std::string &addr, const std::string &svc)
{
    (void)svc;
    auto it = m_gatt.find(addr);
    if (it == m_gatt.end() || it->second.dscDiscovering) return;
    int h = getConnHandle(addr);
    if (h < 0) return;
    it->second.dscDiscovering = true;
    // 全范围发现描述符；uuid 已限定，无需按 svc handle 范围
    ble_gattc_disc_all_dscs(h, 0x0001, 0xFFFF, onGattcDscEvent, (void *)this);
}

// ==================== GAP 回调 — arg 透传自 NimBLE 调用 ====================
// ==================== GAP 事件回调（按角色拆分） ====================
// 三个独立回调分别对应扫描 / Peripheral / Central，arg 类型由各注册点固定，
// 因此回调内部无需运行时 cast 或角色判断，直接按固定类型使用 arg。

int JZBleDriverEsp32::onScanGapEvent(struct ble_gap_event *event, void *arg)
{
    auto *self = g_driver;
    switch (event->type)
    {
    case BLE_GAP_EVENT_DISC:   // arg = this（driver）
    {
        JZBLEDeviceInfo info;
        info.setAddress(addrToString(event->disc.addr.val));
        info.setRssi(event->disc.rssi);
        struct ble_hs_adv_fields f;
        if (ble_hs_adv_parse_fields(&f, event->disc.data, event->disc.length_data) == 0 && f.name && f.name_len > 0)
            info.setName(std::string((const char *)f.name, f.name_len));
        if (self->m_scanAgent)
            JZBLEDeviceDiscoveryAgent::onDeviceDiscovered((JZBLEDeviceDiscoveryAgent *)self->m_scanAgent.object(), info);
        break;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
        self->m_scanning = false;
        if (self->m_scanAgent)
        {
            JZBLEDeviceDiscoveryAgent::onScanFinished((JZBLEDeviceDiscoveryAgent *)self->m_scanAgent.object());
            self->m_scanAgent = JZObjectPointer();
        }
        break;
    }
    return 0;
}

int JZBleDriverEsp32::onPeriphGapEvent(struct ble_gap_event *event, void *arg)
{
    auto *self = g_driver;
    auto *periph = static_cast<JZBLEPeripheral *>(arg);  // arg 固定为 JZBLEPeripheral*
    switch (event->type)
    {
    case BLE_GAP_EVENT_CONNECT:
        // Peripheral：对端连上本机
        if (event->connect.status == 0)
        {
            printf("BLE: client connected, handle=%d\n", event->connect.conn_handle);
            self->m_periphConnHandle = event->connect.conn_handle;
            self->m_periphCtrlPriv = periph;
            JZBleDriver::peripheralOnClientConnected(periph);
        } else {
            printf("BLE: connect failed, status=%d\n", event->connect.status);
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        printf("BLE: client disconnected, reason=%d\n", event->disconnect.reason);
    {
        std::string addr = addrToString(event->disconnect.conn.peer_id_addr.val);
        self->m_periphConnHandle = -1;
        self->m_periphCtrlPriv = nullptr;
        // 重置所有特征的订阅状态（对端已断开）
        for (auto &kv : self->m_periphCharMap) {
            kv.second.subscribedNotify = false;
            kv.second.subscribedIndicate = false;
        }
        JZBleDriver::peripheralOnClientDisconnected(periph);
        break;
    }
    case BLE_GAP_EVENT_SUBSCRIBE:
    case BLE_GAP_EVENT_MTU:
    case BLE_GAP_EVENT_ENC_CHANGE:
        break;  // peripheral 侧暂不处理
    }
    return 0;
}

int JZBleDriverEsp32::onCentralGapEvent(struct ble_gap_event *event, void *arg)
{
    auto *self = g_driver;
    auto *cent = static_cast<JZBLEController *>(arg);  // arg 固定为 JZBLEController*
    switch (event->type)
    {
    case BLE_GAP_EVENT_CONNECT:
    {
        struct ble_gap_conn_desc desc;
        ble_gap_conn_find(event->connect.conn_handle, &desc);
        std::string addr = addrToString(desc.peer_id_addr.val);
        if (event->connect.status == 0)
        {
            self->setConnHandle(addr, event->connect.conn_handle);
            JZBleDriver::centralOnDeviceConnected(cent, addr.c_str());
        }
        else
        {
            self->m_bondCentralByAddr.erase(addr);  // 连接失败，移除路由表项
            JZBleDriver::centralOnError(cent, 0, "GAP connect failed");
        }
        break;
    }
    case BLE_GAP_EVENT_DISCONNECT:
    {
        std::string addr = addrToString(event->disconnect.conn.peer_id_addr.val);
        self->removeConnHandle(addr);
        JZBleDriver::centralOnDeviceDisconnected(cent, addr.c_str());
        break;
    }
    case BLE_GAP_EVENT_NOTIFY_RX:
    {
        // 本机作为 Central 收到对端通知，按 conn_handle + attr_handle 路由到具体服务。
        uint16_t h   = event->notify_rx.conn_handle;
        uint16_t attr = event->notify_rx.attr_handle;
        std::string addr = self->connHandleToAddr(h);
        auto git = self->m_gatt.find(addr);
        if (git != self->m_gatt.end())
        {
            auto hit = git->second.handleMap.find(attr);
            if (hit != git->second.handleMap.end())
            {
                const std::string &svc = hit->second.first;
                auto pit = self->m_svcPtrCache.find(addr + "|" + svc);
                if (pit != self->m_svcPtrCache.end() && event->notify_rx.om)
                {
                    JZByteArray v;
                    v.append((const char *)event->notify_rx.om->om_data, (int)event->notify_rx.om->om_len);
                    JZBLEService::onCharacteristicChanged(pit->second, addr.c_str(), svc.c_str(), v);
                }
            }
        }
        break;
    }
    case BLE_GAP_EVENT_MTU:
    case BLE_GAP_EVENT_ENC_CHANGE:
        break;  // central 侧暂不处理
    }
    return 0;
}

// ==================== GATT 客户端（Central）事件回调 ====================
int JZBleDriverEsp32::onGattcSvcEvent(uint16_t conn_handle, const struct ble_gatt_error *error,
                                        const struct ble_gatt_svc *svc, void *arg)
{
    auto *d = (JZBLEController *)arg;
    std::string addr = d->deviceInfo().address();
    auto *self = g_driver;
    if (error->status == BLE_HS_EDONE)
    {
        JZBleDriver::centralOnServicesDiscovered(d, addr.c_str());
        return 0;
    }
    if (error->status == 0 && svc)
    {
        self->m_gatt[addr].svcs.push_back({ bleUuidToJZ(&svc->uuid.u), svc->start_handle, svc->end_handle });
    }
    return 0;
}

int JZBleDriverEsp32::onGattcCharEvent(uint16_t conn_handle, const struct ble_gatt_error *error,
                                         const struct ble_gatt_chr *chr, void *arg)
{
    auto *d = (JZBLEService *)arg;
    std::string addr = d->deviceAddress;
    std::string svc = d->m_serviceUuid.toString();
    auto *self = g_driver;
    if (error->status == BLE_HS_EDONE)
    {
        // 发现完成：构建 JZLowEnergyCharacteristic 列表并通知上层
        std::vector<JZLowEnergyCharacteristic> list;
        auto &chars = self->m_gatt[addr].chars[svc];
        for (auto &c : chars)
        {
            JZLowEnergyCharacteristic ch;
            ch.setUuid(c.uuid);
            ch.setProperties(c.properties);
            list.push_back(ch);
        }
        // 触发 descriptor 发现（用于后续订阅/描述符读写）
        self->ensureDescriptorsDiscovered(addr, svc);
        JZBLEService::onCharacteristicsDiscovered(d, addr.c_str(), svc.c_str());
        return 0;
    }
    if (error->status == 0 && chr)
    {
        GattCharInfo ci;
        ci.uuid = bleUuidToJZ(&chr->uuid.u);
        ci.valHandle = chr->val_handle;
        ci.properties = chr->properties;
        self->m_gatt[addr].chars[svc].push_back(ci);
        self->m_gatt[addr].handleMap[chr->val_handle] = { svc, ci.uuid.toString() };
    }
    return 0;
}

int JZBleDriverEsp32::onGattcReadEvent(uint16_t conn_handle, const struct ble_gatt_error *error,
                                         struct ble_gatt_attr *attr, void *arg)
{
    auto *d = (JZBLEService *)arg;
    if (!d) return 0;
    std::string addr = d->deviceAddress;
    std::string svc = d->m_serviceUuid.toString();
    JZByteArray val;
    if (error->status == 0 && attr && attr->om)
        val.append((const char *)attr->om->om_data, (int)attr->om->om_len);
    JZBLEService::onCharacteristicRead(d, addr.c_str(), svc.c_str(), val);
    return 0;
}

int JZBleDriverEsp32::onGattcWriteEvent(uint16_t conn_handle, const struct ble_gatt_error *error,
                                          struct ble_gatt_attr *attr, void *arg)
{
    auto *d = (JZBLEService *)arg;
    if (!d) return 0;
    std::string addr = d->deviceAddress;
    std::string svc = d->m_serviceUuid.toString();
    bool ok = (error->status == 0 || error->status == BLE_HS_EDONE);
    JZByteArray val;
    if (attr && attr->om) val.append((const char *)attr->om->om_data, (int)attr->om->om_len);
    JZBLEService::onCharacteristicWritten(d, addr.c_str(), svc.c_str(), val, ok);
    return 0;
}

int JZBleDriverEsp32::onGattcDscEvent(uint16_t conn_handle, const struct ble_gatt_error *error,
                                       uint16_t attr_handle, const struct ble_gatt_dsc *dsc, void *arg)
{
    (void)attr_handle;
    auto *self = static_cast<JZBleDriverEsp32 *>(arg);
    std::string addr = self->connHandleToAddr(conn_handle);
    auto it = self->m_gatt.find(addr);
    if (it == self->m_gatt.end()) return 0;

    if (error->status == BLE_HS_EDONE)
    {
        it->second.dscDiscovering = false;
        auto pend = std::move(it->second.pendingDsc);
        it->second.pendingDsc.clear();
        for (auto &p : pend)
        {
            if (p.type == PendingDsc::Notify)
            {
                std::string key = p.svc + "|" + p.chr;
                auto cit = it->second.cccd.find(key);
                if (cit != it->second.cccd.end())
                    self->writeCccd(conn_handle, cit->second, p.enable);
            }
            else if (p.type == PendingDsc::Read)
            {
                uint16_t dh = self->findDscHandle(addr, p.svc, p.chr, p.desc);
                if (dh) ble_gattc_read(conn_handle, dh, onGattcReadDscEvent, (void *)p.d);
            }
            else if (p.type == PendingDsc::Write)
            {
                uint16_t dh = self->findDscHandle(addr, p.svc, p.chr, p.desc);
                if (dh) ble_gattc_write_flat(conn_handle, dh, p.value.data(), p.value.size(), onGattcWriteDscEvent, (void *)p.d);
            }
        }
        return 0;
    }
    if (error->status == 0 && dsc)
    {
        // ble_gatt_dsc 仅有 handle/uuid，无所属特征信息。
        // 描述符 handle 必大于其所属特征的 val_handle，且介于本特征与下一特征之间。
        // 故归属到 val_handle < dsc->handle 且最接近的特征。
        JZBluetoothUuid du = bleUuidToJZ(&dsc->uuid.u);
        uint16_t bestVal = 0;
        std::string bestKey;
        for (auto &sc : it->second.chars)
            for (auto &ch : sc.second)
                if (ch.valHandle < dsc->handle && ch.valHandle >= bestVal) {
                    bestVal = ch.valHandle;
                    bestKey = sc.first + "|" + ch.uuid.toString();
                }
        if (!bestKey.empty()) {
            it->second.dscs[bestKey].push_back({ du, dsc->handle });
            if (du.toUInt16() == 0x2902) // CCCD
                it->second.cccd[bestKey] = dsc->handle;
        }
    }
    return 0;
}

int JZBleDriverEsp32::onGattcReadDscEvent(uint16_t conn_handle, const struct ble_gatt_error *error,
                                            struct ble_gatt_attr *attr, void *arg)
{
    auto *d = (JZBLEService *)arg;
    if (!d) return 0;
    std::string addr = d->deviceAddress, svc = d->m_serviceUuid.toString();
    JZByteArray val;
    if (error->status == 0 && attr && attr->om)
        val.append((const char *)attr->om->om_data, (int)attr->om->om_len);
    JZBLEService::onDescriptorRead(d, addr.c_str(), svc.c_str(), "", val);
    return 0;
}
int JZBleDriverEsp32::onGattcWriteDscEvent(uint16_t conn_handle, const struct ble_gatt_error *error,
                                             struct ble_gatt_attr *attr, void *arg)
{
    auto *d = (JZBLEService *)arg;
    if (!d) return 0;
    std::string addr = d->deviceAddress, svc = d->m_serviceUuid.toString();
    bool ok = (error->status == 0 || error->status == BLE_HS_EDONE);
    JZByteArray val;
    if (attr && attr->om) val.append((const char *)attr->om->om_data, (int)attr->om->om_len);
    JZBLEService::onDescriptorWritten(d, addr.c_str(), svc.c_str(), "", val, ok);
    return 0;
}
int JZBleDriverEsp32::onGattcAck(uint16_t, const struct ble_gatt_error *, struct ble_gatt_attr *, void *)
{
    return 0;
}

// ==================== 构造/析构 ====================
JZBleDriverEsp32::JZBleDriverEsp32() : m_ownAddrType(BLE_OWN_ADDR_PUBLIC), m_advertState(State::None), m_scanning(false)
{
    g_driver = this;
    m_periphGatt = new PeriphGatt();

    // 配置 NimBLE Host（在 nimble_port_init() 之前设置）
    ble_hs_cfg.reset_cb = onHostReset;
    ble_hs_cfg.sync_cb = onHostSync;
    ble_hs_cfg.gatts_register_cb = gatt_svr_register_cb;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.store_read_cb = storeReadCb;
    ble_hs_cfg.store_write_cb = storeWriteCb;
    ble_hs_cfg.store_delete_cb = storeDeleteCb;

    // 启用安全管理器（配对/绑定）
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm    = 0;
    ble_hs_cfg.sm_sc      = 1;
    ble_hs_cfg.sm_io_cap  = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_our_key_dist   = BLE_SM_PAIR_KEY_DIST_ENC;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC
                                  | BLE_SM_PAIR_KEY_DIST_ID;

    // nimble_port_init() 延迟到 startAdvertising() 中调用，
    // 确保 addService/addCharacteristic 先于 host sync 完成注册
}

JZBleDriverEsp32::~JZBleDriverEsp32() 
{ 
    delete m_periphGatt; 
    g_driver = nullptr;
}

void JZBleDriverEsp32::onHostReset(int)
{
    // 复位时可做清理，这里保持简单
}

// NimBLE GATT 注册回调（记录服务/特征/描述符注册信息）
static void gatt_svr_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg)
{
    (void)arg;
    char buf[BLE_UUID_STR_LEN];
    switch (ctxt->op) {
    case BLE_GATT_REGISTER_OP_SVC:
        printf("BLE: register svc %s handle=%d\n",
               ble_uuid_to_str(ctxt->svc.svc_def->uuid, buf), ctxt->svc.handle);
        break;
    case BLE_GATT_REGISTER_OP_CHR:
        printf("BLE: register chr %s def_handle=%d val_handle=%d\n",
               ble_uuid_to_str(ctxt->chr.chr_def->uuid, buf),
               ctxt->chr.def_handle, ctxt->chr.val_handle);
        break;
    case BLE_GATT_REGISTER_OP_DSC:
        printf("BLE: register dsc %s handle=%d\n",
               ble_uuid_to_str(ctxt->dsc.dsc_def->uuid, buf), ctxt->dsc.handle);
        break;
    default:
        break;
    }
}

void JZBleDriverEsp32::onHostSync()
{
    auto *self = g_driver;
    if (!self) 
        return;

    int rc = ble_hs_util_ensure_addr(0); // 优先公钥地址
    if (rc != 0) {
        printf("BLE: ble_hs_util_ensure_addr failed, rc=%d\n", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &self->m_ownAddrType);
    if (rc != 0) self->m_ownAddrType = BLE_OWN_ADDR_PUBLIC;

    self->m_advertState = State::Started;
    printf("BLE: host synced\n");

    printf("BLE: onHostSync pendingAdv=%d\n", self->m_pendingAdv);

    // 处理延迟广播请求
    if (self->m_pendingAdv && self->m_pendingAdvPeriph)
    {
        self->m_periphConnHandle = -1;
        self->m_periphCtrlPriv = self->m_pendingAdvPeriph;

        // 广播数据
        struct ble_hs_adv_fields f; memset(&f, 0, sizeof(f));
        f.name = (uint8_t *)self->m_pendingAdvName.c_str();
        f.name_len = self->m_pendingAdvName.size();
        f.name_is_complete = 1;
        f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

        std::vector<ble_uuid16_t> u16; u16.reserve(self->m_pendingAdvUuids.size());
        std::vector<ble_uuid128_t> u128; u128.reserve(self->m_pendingAdvUuids.size());
        for (auto &u : self->m_pendingAdvUuids) {
            std::string s = u.toString();
            ble_uuid_any_t tmp; uuidFromString(s, &tmp);
            if (tmp.u.type == BLE_UUID_TYPE_16) u16.push_back(*(ble_uuid16_t*)&tmp.u);
            else if (tmp.u.type == BLE_UUID_TYPE_128) u128.push_back(*(ble_uuid128_t*)&tmp.u);
        }
        if (!u16.empty()) { f.uuids16 = u16.data(); f.num_uuids16 = u16.size(); f.uuids16_is_complete = 1; }
        if (!u128.empty()) { f.uuids128 = u128.data(); f.num_uuids128 = u128.size(); f.uuids128_is_complete = 1; }

        ble_gap_adv_set_fields(&f);

        struct ble_gap_adv_params p; memset(&p, 0, sizeof(p));
        p.conn_mode = BLE_GAP_CONN_MODE_UND; p.disc_mode = BLE_GAP_DISC_MODE_GEN;
        ble_gap_adv_start(self->m_ownAddrType, nullptr, BLE_HS_FOREVER, &p,
                          onPeriphGapEvent, self->m_pendingAdvPeriph);

        self->m_pendingAdv = false;
        self->m_pendingAdvName.clear();
        self->m_pendingAdvUuids.clear();
        self->m_pendingAdvPeriph = nullptr;
    }
}

// ==================== 绑定密钥 store（驱动不持久化） ====================
// 密钥仅缓存在 m_bondStore（本次会话），配对时上报上层，重连时由上层回灌。
static std::string bondStoreKey(int obj_type, const std::string &addr)
{
    return std::to_string(obj_type) + ":" + addr;
}

JZBleDriverEsp32::JZBleBondKey JZBleDriverEsp32::secToBondKey(int obj_type, const struct ble_store_value_sec *sec)
{
    JZBleBondKey k;
    k.objType      = (uint8_t)obj_type;
    k.peerAddr      = addrToString(sec->peer_addr.val);
    k.peerAddrType  = sec->peer_addr.type;
    k.keySize       = sec->key_size;
    k.ediv          = sec->ediv;
    k.randNum       = sec->rand_num;
    memcpy(k.ltk,  sec->ltk,  16); k.ltkPresent  = sec->ltk_present;
    memcpy(k.irk,  sec->irk,  16); k.irkPresent  = sec->irk_present;
    memcpy(k.csrk, sec->csrk, 16); k.csrkPresent = sec->csrk_present;
    k.authenticated = sec->authenticated;
    k.secureConnections = sec->sc;
    return k;
}

void JZBleDriverEsp32::bondKeyToSec(const JZBleBondKey &k, struct ble_store_value_sec *sec)
{
    memset(sec, 0, sizeof(*sec));
    addrFromString(k.peerAddr, sec->peer_addr.val);
    sec->peer_addr.type = k.peerAddrType;
    sec->key_size    = k.keySize;
    sec->ediv        = k.ediv;
    sec->rand_num    = k.randNum;
    memcpy(sec->ltk,  k.ltk,  16); sec->ltk_present  = k.ltkPresent;
    memcpy(sec->irk,  k.irk,  16); sec->irk_present  = k.irkPresent;
    memcpy(sec->csrk, k.csrk, 16); sec->csrk_present = k.csrkPresent;
    sec->authenticated = k.authenticated;
    sec->sc            = k.secureConnections;
}

int JZBleDriverEsp32::storeReadCb(int obj_type, const union ble_store_key *key, union ble_store_value *dst)
{
    if (!key || !dst) return BLE_HS_EINVAL;
    std::string addr;
    if (obj_type == BLE_STORE_OBJ_TYPE_PEER_SEC || obj_type == BLE_STORE_OBJ_TYPE_OUR_SEC)
        addr = addrToString(key->sec.peer_addr.val);
    else
        return BLE_HS_ENOENT;   // CCCD 等不缓存，按未找到处理

    auto *self = g_driver;
    auto it = self->m_bondStore.find(bondStoreKey(obj_type, addr));
    if (it == self->m_bondStore.end()) return BLE_HS_ENOENT;

    bondKeyToSec(it->second, &dst->sec);
    return 0;
}

int JZBleDriverEsp32::storeWriteCb(int obj_type, const union ble_store_value *val)
{
    auto *self = g_driver;
    if (obj_type == BLE_STORE_OBJ_TYPE_PEER_SEC || obj_type == BLE_STORE_OBJ_TYPE_OUR_SEC) {
        std::string addr = addrToString(val->sec.peer_addr.val);
        JZBleBondKey key = secToBondKey(obj_type, &val->sec);
        self->m_bondStore[bondStoreKey(obj_type, addr)] = key;
        // 分别上报 PEER_SEC(对端密钥) 与 OUR_SEC(本机密钥)：两者都需持久化，
        // 外设角色重连加密依赖 OUR_SEC，故不跳过。按对端地址路由到对应 Central。
        auto bit = self->m_bondCentralByAddr.find(addr);
        if (bit != self->m_bondCentralByAddr.end())
            JZBleDriver::centralOnBondKeyReported(bit->second, addr, key);
        return 0;
    }
    return 0;   // 其他类型（CCCD 等）忽略，不持久化
}

int JZBleDriverEsp32::storeDeleteCb(int obj_type, const union ble_store_key *key)
{
    auto *self = g_driver;
    std::string addr;
    if (obj_type == BLE_STORE_OBJ_TYPE_PEER_SEC || obj_type == BLE_STORE_OBJ_TYPE_OUR_SEC)
        addr = addrToString(key->sec.peer_addr.val);
    else
        return BLE_HS_ENOENT;
    self->m_bondStore.erase(bondStoreKey(obj_type, addr));
    return 0;
}

void JZBleDriverEsp32::setBondKey(const std::string &addr, const JZBleBondKey &key)
{
    // 写入内存缓存，重连加密时 storeReadCb 据此取回；不调用 ble_store_write 以避免递归/重复上报。
    // 按 key.objType 定位（PEER_SEC / OUR_SEC 各自独立），外设角色重连加密依赖 OUR_SEC。
    m_bondStore[bondStoreKey(key.objType, addr)] = key;
}

void JZBleDriverEsp32::requestPairing(const std::string &addr)
{
    int h = getConnHandle(addr);
    if (h >= 0) ble_gap_security_initiate(h);
}

// ==================== Central ====================
void JZBleDriverEsp32::startScan(JZBLEDeviceDiscoveryAgent *agent)
{
    JZ_ASSERT_X(!m_scanAgent, "ble allready start scan");
    m_scanAgent = agent->guardPointer();
    m_scanning = true;
    struct ble_gap_disc_params p; memset(&p, 0, sizeof(p)); p.passive = 1;
    ble_gap_disc(m_ownAddrType, BLE_HS_FOREVER, &p, onScanGapEvent, this);
}
void JZBleDriverEsp32::stopScan(JZBLEDeviceDiscoveryAgent *agent)
{
    JZ_ASSERT_X(agent == m_scanAgent.object(), "stopScan invalid param");
    m_scanning = false;
    ble_gap_disc_cancel();
}
bool JZBleDriverEsp32::isScanning(JZBLEDeviceDiscoveryAgent *agent)
{
    return agent ? (agent == (JZBLEDeviceDiscoveryAgent *)m_scanAgent.object()) : false;
}

void JZBleDriverEsp32::connectToDevice(JZBLEController *d, const std::vector<JZBleBondKey>& keys)
{
    // 按对端地址记录上报目标，支持多 Central 并发（避免单一目标误路由）
    std::string addr = d->deviceInfo().address();
    m_bondCentralByAddr[addr] = d;
    // 连接前回灌该设备的绑定密钥，重连时驱动据此恢复加密通道
    for (const auto &key : keys)
        m_bondStore[bondStoreKey(key.objType, addr)] = key;
    ble_addr_t peer;
    peer.type = BLE_ADDR_PUBLIC;
    addrFromString(addr, peer.val);
    ble_gap_connect(m_ownAddrType, &peer, BLE_HS_FOREVER, nullptr, onCentralGapEvent, d);
}
void JZBleDriverEsp32::disconnectFromDevice(JZBLEController *d)
{
    m_bondCentralByAddr.erase(d->deviceInfo().address());
    int h = getConnHandle(d->deviceInfo().address());
    if (h >= 0) ble_gap_terminate(h, BLE_ERR_REM_USER_CONN_TERM);
}
bool JZBleDriverEsp32::isConnected(JZBLEController *d) { return getConnHandle(d->deviceInfo().address()) >= 0; }

void JZBleDriverEsp32::discoverServices(const std::string &addr, JZBLEController *d)
{
    int h = getConnHandle(addr);
    if (h < 0) return;
    // 清空该地址的旧 GATT 缓存，避免重连/重复发现时服务与特征重复累积。
    // 后续 discoverCharacteristics 会重新填充 chars/handleMap，并重建 m_svcPtrCache。
    m_gatt[addr] = ConnGatt();
    ble_gattc_disc_all_svcs(h, onGattcSvcEvent, (void *)d);
}
std::vector<JZBluetoothUuid> JZBleDriverEsp32::services(const std::string &addr)
{
    auto it = m_gatt.find(addr);
    if (it == m_gatt.end()) return {};
    std::vector<JZBluetoothUuid> r;
    for (auto &s : it->second.svcs) r.push_back(s.uuid);
    return r;
}

void JZBleDriverEsp32::discoverCharacteristics(const std::string &addr, const std::string &svc, JZBLEService *d)
{
    int h = getConnHandle(addr);
    if (h < 0) return;
    m_svcPtrCache[addr + "|" + svc] = d;   // 通知路由用
    ble_uuid_any_t u; uuidFromString(svc, &u);
    // uuid 已限定特征，handle 范围用全范围即可，避免 svc 字符串格式不一致导致匹配失败
    ble_gattc_disc_chrs_by_uuid(h, 0x0001, 0xFFFF, &u.u, onGattcCharEvent, (void *)d);
}
std::vector<JZLowEnergyCharacteristic> JZBleDriverEsp32::characteristics(const std::string &addr, const std::string &svc)
{
    auto it = m_gatt.find(addr);
    if (it == m_gatt.end()) return {};
    auto cit = it->second.chars.find(svc);
    if (cit == it->second.chars.end()) return {};
    std::vector<JZLowEnergyCharacteristic> r;
    for (auto &c : cit->second) {
        JZLowEnergyCharacteristic ch;
        ch.setUuid(c.uuid);
        ch.setProperties(c.properties);
        r.push_back(ch);
    }
    return r;
}

void JZBleDriverEsp32::readCharacteristic(const std::string &addr, const std::string &svc, const std::string &c, JZBLEService *d)
{
    int h = getConnHandle(addr); if (h < 0) return;
    uint16_t vh = findCharValHandle(addr, svc, c);
    if (vh == 0) return;
    ble_gattc_read(h, vh, onGattcReadEvent, (void *)d);
}
void JZBleDriverEsp32::writeCharacteristic(const std::string &addr, const std::string &svc, const std::string &c, const JZByteArray &v, bool nrs, JZBLEService *d)
{
    int h = getConnHandle(addr); if (h < 0) return;
    uint16_t vh = findCharValHandle(addr, svc, c);
    if (vh == 0) return;
    if (nrs) ble_gattc_write_no_rsp_flat(h, vh, v.data(), v.size());
    else     ble_gattc_write_flat(h, vh, v.data(), v.size(), onGattcWriteEvent, (void *)d);
}
void JZBleDriverEsp32::enableNotification(const std::string &addr, const std::string &svc, const std::string &c, bool en, JZBLEService *d)
{
    int h = getConnHandle(addr); if (h < 0) return;
    std::string key = svc + "|" + normalizeUuid(c);
    auto it = m_gatt.find(addr);
    if (it == m_gatt.end()) return;
    auto cit = it->second.cccd.find(key);
    if (cit != it->second.cccd.end()) {
        writeCccd(h, cit->second, en);
    } else {
        PendingDsc p; p.type = PendingDsc::Notify; p.svc = svc; p.chr = normalizeUuid(c); p.enable = en; p.d = d;
        it->second.pendingDsc.push_back(p);
        ensureDescriptorsDiscovered(addr, svc);
    }
}
void JZBleDriverEsp32::readDescriptor(const std::string &addr, const std::string &svc, const std::string &c, const std::string &desc, JZBLEService *d)
{
    int h = getConnHandle(addr); if (h < 0) return;
    uint16_t dh = findDscHandle(addr, svc, c, desc);
    if (dh) { ble_gattc_read(h, dh, onGattcReadDscEvent, (void *)d); return; }
    auto it = m_gatt.find(addr);
    if (it == m_gatt.end()) return;
    PendingDsc p; p.type = PendingDsc::Read; p.svc = svc; p.chr = c; p.desc = desc; p.d = d;
    it->second.pendingDsc.push_back(p);
    ensureDescriptorsDiscovered(addr, svc);
}
void JZBleDriverEsp32::writeDescriptor(const std::string &addr, const std::string &svc, const std::string &c, const std::string &desc, const JZByteArray &v, JZBLEService *d)
{
    int h = getConnHandle(addr); if (h < 0) return;
    uint16_t dh = findDscHandle(addr, svc, c, desc);
    if (dh) { ble_gattc_write_flat(h, dh, v.data(), v.size(), onGattcWriteDscEvent, (void *)d); return; }
    auto it = m_gatt.find(addr);
    if (it == m_gatt.end()) return;
    PendingDsc p; p.type = PendingDsc::Write; p.svc = svc; p.chr = c; p.desc = desc; p.value = v; p.d = d;
    it->second.pendingDsc.push_back(p);
    ensureDescriptorsDiscovered(addr, svc);
}

// ==================== Peripheral ====================
void JZBleDriverEsp32::buildPeriphGatt()
{
    m_periphGatt->svcUuids.clear();
    m_periphGatt->chrUuids.clear();
    m_periphGatt->chrDefs.clear();
    m_periphGatt->svcDefs.clear();

    std::map<std::string, std::vector<std::string>> svcChars;
    for (auto &kv : m_periphCharMap)
        svcChars[kv.second.svcUuid].push_back(kv.first);

    // resize 一把创建所有元素，地址终生不变，NimBLE 存的裸指针永远安全
    size_t svcCount = svcChars.size();
    size_t totalChars = m_periphCharMap.size();
    m_periphGatt->svcUuids.resize(svcCount);
    m_periphGatt->chrUuids.resize(totalChars);
    m_periphGatt->chrDefs.resize(totalChars + svcCount);  // chars + 每服务终止符
    m_periphGatt->svcDefs.resize(svcCount + 1);           // svcs + 终止符

    size_t svcIdx = 0, chrUuidIdx = 0, chrDefIdx = 0, svcDefIdx = 0;

    for (auto &sc : svcChars)
    {
        uuidFromString(sc.first, &m_periphGatt->svcUuids[svcIdx]);

        size_t chrStartIdx = chrDefIdx;
        for (auto &cuuid : sc.second)
        {
            uuidFromString(cuuid, &m_periphGatt->chrUuids[chrUuidIdx]);

            int props = m_periphCharMap[cuuid].properties;
            ble_gatt_chr_flags nimFlags = 0;
            if (props & 0x01) nimFlags |= BLE_GATT_CHR_F_READ;
            if (props & 0x02) nimFlags |= BLE_GATT_CHR_F_WRITE;
            if (props & 0x04) nimFlags |= BLE_GATT_CHR_F_NOTIFY;
            if (props & 0x08) nimFlags |= BLE_GATT_CHR_F_INDICATE;
            if (props & 0x10) nimFlags |= BLE_GATT_CHR_F_WRITE_NO_RSP;

            ble_gatt_chr_def cd = {};
            cd.uuid = &m_periphGatt->chrUuids[chrUuidIdx].u;
            cd.access_cb = onGattAccess;
            cd.val_handle = &m_periphCharMap[cuuid].valHandle;
            cd.flags = nimFlags;
            cd.min_key_size = 0;
            m_periphGatt->chrDefs[chrDefIdx] = cd;

            chrUuidIdx++;
            chrDefIdx++;
        }
        m_periphGatt->chrDefs[chrDefIdx] = ble_gatt_chr_def{};  // 终止符
        chrDefIdx++;

        ble_gatt_svc_def sd = {};
        sd.type = BLE_GATT_SVC_TYPE_PRIMARY;
        sd.uuid = &m_periphGatt->svcUuids[svcIdx].u;
        sd.characteristics = &m_periphGatt->chrDefs[chrStartIdx];
        m_periphGatt->svcDefs[svcDefIdx] = sd;

        svcIdx++;
        svcDefIdx++;
    }
    m_periphGatt->svcDefs[svcDefIdx] = ble_gatt_svc_def{};  // 终止符
}

void JZBleDriverEsp32::startAdvertising(const std::string &name, const std::vector<JZBluetoothUuid> &uuids, JZBLEPeripheral *d)
{
    if (m_advertState != State::None) {
        printf("BLE: startAdvertising: state=%d, expected None\n", (int)m_advertState);
        return;
    }    

    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        printf("BLE: nimble_port_init failed: %s\n", esp_err_to_name(ret));
        return;
    }

    // GAP 服务初始化（同 gap_init() 例子）
    ble_svc_gap_init();
    ble_svc_gap_device_name_set(name.c_str());

    // GATT 服务注册（必须在 host task 启动之前完成）
    ble_svc_gatt_init();
    buildPeriphGatt();
    int rc2 = ble_gatts_count_cfg(m_periphGatt->svcDefs.data());
    if (rc2 == 0)
        rc2 = ble_gatts_add_svcs(m_periphGatt->svcDefs.data());
    if (rc2 != 0) {
        printf("BLE: GATT service register failed, rc=%d\n", rc2);
        return;
    }
    printf("BLE: GATT services registered OK, count=%d\n", (int)m_periphGatt->svcDefs.size());

    m_periphCtrlPriv = d;
    m_advertState = State::Starting;

    // 在 nimble_port_freertos_init 之前设 pending，
    // 防止 NimBLE 任务在 pending 设完前就触发 onHostSync
    m_pendingAdv = true;
    m_pendingAdvName = name;
    m_pendingAdvUuids = uuids;
    m_pendingAdvPeriph = d;
    nimble_port_freertos_init([](void *) { nimble_port_run(); });
}

void JZBleDriverEsp32::stopAdvertising()
{
    if (m_advertState == State::None) return;

    printf("BLE: stop + deinit NimBLE\n");
    ble_gap_adv_stop();
    nimble_port_stop();
    nimble_port_deinit();

    m_advertState = State::None;
    m_pendingAdv = false;
    m_pendingAdvName.clear();
    m_pendingAdvUuids.clear();
    m_pendingAdvPeriph = nullptr;
    m_periphConnHandle = -1;
}

void JZBleDriverEsp32::addPeripheralCharacteristic(const std::string &svcUuid, const std::string &charUuid, int properties)
{
    if (m_advertState != State::None) {
        printf("BLE: cannot add char after NimBLE started (svc=%s chr=%s)\n", svcUuid.c_str(), charUuid.c_str());
        return;
    }
    m_periphCharMap[charUuid] = { svcUuid, 0, properties, JZByteArray() };
    printf("BLE: add char svc=%s chr=%s props=0x%x\n", svcUuid.c_str(), charUuid.c_str(), properties);
}

int JZBleDriverEsp32::sendPeriphData(int connHandle, uint16_t valHandle, const JZByteArray &value, bool indicate)
{
    if (connHandle < 0 || valHandle == 0 || value.isEmpty()) return -1;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(value.data(), (uint16_t)value.size());
    if (!om) {
        printf("BLE: sendPeriphData mbuf alloc failed, size=%d\n", (int)value.size());
        return -1;
    }
    int rc = indicate
        ? ble_gatts_indicate_custom(connHandle, valHandle, om)
        : ble_gatts_notify_custom(connHandle, valHandle, om);
    if (rc != 0) {
        printf("BLE: sendPeriphData notify/indicate failed, rc=%d\n", rc);
        os_mbuf_free_chain(om);
    }
    return rc;
}

// GATT access 回调：处理 Peripheral 端的特征读写
int JZBleDriverEsp32::onGattAccess(uint16_t conn_handle, uint16_t attr_handle,
                                    struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)arg;
    auto *self = g_driver;
    if (!self || !self->m_periphCtrlPriv) return BLE_ATT_ERR_UNLIKELY;

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR && ctxt->om)
    {
        for (auto &kv : self->m_periphCharMap)
        {
            if (kv.second.valHandle == attr_handle)
            {
                JZByteArray val;
                val.append((const char *)ctxt->om->om_data, (int)ctxt->om->om_len);
                kv.second.value = val;   // 保存以便后续读取
                JZBleDriver::peripheralOnCharWritten(self->m_periphCtrlPriv,
                                               JZBluetoothUuid(kv.first), val);
                return 0;
            }
        }
        printf("BLE: onGattAccess WRITE: attr_handle %d not found in charMap\n", attr_handle);
        return BLE_ATT_ERR_UNLIKELY;
    }
    else if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR)
    {
        for (auto &kv : self->m_periphCharMap)
        {
            if (kv.second.valHandle == attr_handle)
            {
                int rc = os_mbuf_append(ctxt->om, kv.second.value.data(), kv.second.value.size());
                return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
            }
        }
        printf("BLE: onGattAccess READ: attr_handle %d not found in charMap\n", attr_handle);
        return BLE_ATT_ERR_UNLIKELY;
    }
    else if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_DSC && ctxt->om)
    {
        // CCCD 写入：记录对端订阅状态（notify / indicate）
        uint16_t val = 0;
        if (ctxt->om->om_len >= 1) val = ctxt->om->om_data[0];
        if (ctxt->om->om_len >= 2) val |= (uint16_t)(ctxt->om->om_data[1] << 8);
        // CCCD 归属 val_handle < attr_handle 且最接近的特征
        uint16_t bestVal = 0; std::string bestKey;
        for (auto &kv : self->m_periphCharMap)
            if (kv.second.valHandle < attr_handle && kv.second.valHandle >= bestVal) {
                bestVal = kv.second.valHandle; bestKey = kv.first;
            }
        if (!bestKey.empty()) {
            auto &ci = self->m_periphCharMap[bestKey];
            ci.subscribedNotify   = (val & 0x0001) != 0;
            ci.subscribedIndicate = (val & 0x0002) != 0;
        }
        return 0;
    }
    else if (ctxt->op == BLE_GATT_ACCESS_OP_READ_DSC)
    {
        // CCCD 读回当前订阅状态
        uint16_t bestVal = 0; std::string bestKey;
        for (auto &kv : self->m_periphCharMap)
            if (kv.second.valHandle < attr_handle && kv.second.valHandle >= bestVal) {
                bestVal = kv.second.valHandle; bestKey = kv.first;
            }
        if (!bestKey.empty()) {
            auto &ci = self->m_periphCharMap[bestKey];
            uint16_t v = (ci.subscribedIndicate ? 0x0002 : 0) | (ci.subscribedNotify ? 0x0001 : 0);
            int rc = os_mbuf_append(ctxt->om, &v, sizeof(v));
            return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        return BLE_ATT_ERR_UNLIKELY;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

void JZBleDriverEsp32::peripheralSendNotify(const JZBluetoothUuid &charUuid, const JZByteArray &value)
{
    auto it = m_periphCharMap.find(charUuid.toString());
    if (it == m_periphCharMap.end()) return;
    if (!it->second.subscribedNotify) return;   // 对端未订阅 notify，跳过
    sendPeriphData(m_periphConnHandle, it->second.valHandle, value, false);
}

void JZBleDriverEsp32::peripheralSendIndicate(const JZBluetoothUuid &charUuid, const JZByteArray &value)
{
    auto it = m_periphCharMap.find(charUuid.toString());
    if (it == m_periphCharMap.end()) return;
    if (!it->second.subscribedIndicate) return;  // 对端未订阅 indicate，跳过
    sendPeriphData(m_periphConnHandle, it->second.valHandle, value, true);
}
