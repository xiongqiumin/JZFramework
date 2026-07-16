#include "JZBleDriverEsp32.h"
#include "JZBLEService.h"
#include "JZBLECentral.h"
#include "JZBLEPeripheral.h"
#include "JZBLEDeviceDiscoveryAgent.h"
#include "JZStd.h"

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

// Peripheral GATT 定义（必须长期存活，不能为局部变量）
struct PeriphGatt {
    std::vector<ble_uuid_any_t>   svcUuids;
    std::vector<ble_uuid_any_t>   chrUuids;
    std::vector<ble_gatt_chr_def> chrDefs;
    std::vector<ble_gatt_svc_def> svcDefs;
    bool registered = false;
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
int JZBleDriverEsp32::onGapEvent(struct ble_gap_event *event, void *arg)
{
    auto *self = static_cast<JZBleDriverEsp32 *>(JZBleDriver::instance());

    switch (event->type)
    {
    case BLE_GAP_EVENT_DISC:   // arg = this（扫描）
    {
        JZBluetoothDeviceInfo info;
        info.setAddress(addrToString(event->disc.addr.val));
        info.setRssi(event->disc.rssi);
        struct ble_hs_adv_fields f;
        if (ble_hs_adv_parse_fields(&f, event->disc.data, event->disc.length_data) == 0 && f.name && f.name_len > 0)
            info.setName(std::string((const char *)f.name, f.name_len));
        for (auto *d : self->m_agents)
            JZBLEDeviceDiscoveryAgentPrivate::onDeviceDiscovered(d, info);
        break;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
        self->m_scanning = false;
        for (auto *d : self->m_agents)
            JZBLEDeviceDiscoveryAgentPrivate::onScanFinished(d);
        break;

    case BLE_GAP_EVENT_CONNECT:
    {
        struct ble_gap_conn_desc desc;
        ble_gap_conn_find(event->connect.conn_handle, &desc);
        std::string addr = addrToString(desc.peer_id_addr.val);

        auto *periph = (JZBLEPeripheral *)arg;
        auto *cent   = (JZBLECentral *)arg;
        if (self->m_periphSet.count(periph))
        {
            // Peripheral：对端连上本机
            if (event->connect.status == 0)
            {
                self->m_periphConnHandle = event->connect.conn_handle;
                self->m_periphCtrlPriv = periph;
                JZBleDriver::peripheralOnClientConnected(periph);
            }
        }
        else if (self->m_centralSet.count(cent))
        {
            // Central：本机连上对端
            if (event->connect.status == 0)
            {
                self->setConnHandle(addr, event->connect.conn_handle);
                JZBleDriver::centralOnDeviceConnected(cent, addr.c_str());
            }
            else
                JZBleDriver::centralOnError(cent, 0, "GAP connect failed");
        }
        break;
    }
    case BLE_GAP_EVENT_DISCONNECT:
    {
        std::string addr = addrToString(event->disconnect.conn.peer_id_addr.val);
        auto *periph = (JZBLEPeripheral *)arg;
        auto *cent   = (JZBLECentral *)arg;
        if (self->m_periphSet.count(periph))
        {
            self->m_periphSet.erase(periph);
            self->m_periphConnHandle = -1;
            self->m_periphCtrlPriv = nullptr;
            JZBleDriver::peripheralOnClientDisconnected(periph);
        }
        else if (self->m_centralSet.count(cent))
        {
            self->m_centralSet.erase(cent);
            self->removeConnHandle(addr);
            JZBleDriver::centralOnDeviceDisconnected(cent, addr.c_str());
        }
        break;
    }
    case BLE_GAP_EVENT_NOTIFY_RX:
    {
        // arg 是连接回调（Central 的 JZBLECentral*）。按 conn_handle + attr_handle 路由到具体服务。
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
                    std::string v((const char *)event->notify_rx.om->om_data, event->notify_rx.om->om_len);
                    JZBLEService::onCharacteristicChanged(pit->second, addr.c_str(), svc.c_str(), v.c_str());
                }
            }
        }
        break;
    }
    case BLE_GAP_EVENT_SUBSCRIBE:
    case BLE_GAP_EVENT_MTU:
        break;

    case BLE_GAP_EVENT_SECURITY:
    {
        // 配对/绑定状态机事件。Just Works（无 io_cap）下无需交互，返回 0 让 SMP 继续；
        // 若改用密码配对，需在此根据 event->security.state 处理 PASSKEY 显示/确认。
        // 密钥本身由 storeWriteCb 在 SMP 过程中写入并上报，无需在此处理。
        break;
    }
    case BLE_GAP_EVENT_ENC_CHANGE:
    {
        // 链路加密状态变化：配对完成或重连用已存 LTK 恢复加密时触发。
        // 此时加密已建立，对应绑定密钥此前已通过 sigBondKeyReported 上报。
        break;
    }
    }
    return 0;
}

// ==================== GATT 客户端（Central）事件回调 ====================
int JZBleDriverEsp32::onGattcSvcEvent(uint16_t conn_handle, const struct ble_gatt_error *error,
                                        const struct ble_gatt_svc *svc, void *arg)
{
    auto *d = (JZBLECentral *)arg;
    std::string addr = d->deviceInfo().address();
    auto *self = static_cast<JZBleDriverEsp32 *>(JZBleDriver::instance());
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
    auto *self = static_cast<JZBleDriverEsp32 *>(JZBleDriver::instance());
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
    std::string val;
    if (error->status == 0 && attr && attr->om)
        val.assign((const char *)attr->om->om_data, attr->om->om_len);
    JZBLEService::onCharacteristicRead(d, addr.c_str(), svc.c_str(), val.c_str());
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
    std::string val;
    if (attr && attr->om) val.assign((const char *)attr->om->om_data, attr->om->om_len);
    JZBLEService::onCharacteristicWritten(d, addr.c_str(), svc.c_str(), val.c_str(), ok);
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
    std::string val;
    if (error->status == 0 && attr && attr->om)
        val.assign((const char *)attr->om->om_data, attr->om->om_len);
    JZBLEService::onDescriptorRead(d, addr.c_str(), svc.c_str(), "", val.c_str());
    return 0;
}
int JZBleDriverEsp32::onGattcWriteDscEvent(uint16_t conn_handle, const struct ble_gatt_error *error,
                                             struct ble_gatt_attr *attr, void *arg)
{
    auto *d = (JZBLEService *)arg;
    if (!d) return 0;
    std::string addr = d->deviceAddress, svc = d->m_serviceUuid.toString();
    bool ok = (error->status == 0 || error->status == BLE_HS_EDONE);
    std::string val;
    if (attr && attr->om) val.assign((const char *)attr->om->om_data, attr->om->om_len);
    JZBLEService::onDescriptorWritten(d, addr.c_str(), svc.c_str(), "", val.c_str(), ok);
    return 0;
}
int JZBleDriverEsp32::onGattcAck(uint16_t, const struct ble_gatt_error *, struct ble_gatt_attr *, void *)
{
    return 0;
}

// ==================== 构造/析构 ====================
JZBleDriverEsp32::JZBleDriverEsp32() : m_scanning(false), m_advertising(false), m_ownAddrType(BLE_OWN_ADDR_PUBLIC)
{
    m_periphGatt = new PeriphGatt();
    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    esp_bt_controller_config_t c = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_bt_controller_init(&c); esp_bt_controller_enable(ESP_BT_MODE_BLE);
    nimble_port_init();
    ble_hs_cfg.reset_cb = onHostReset;
    ble_hs_cfg.sync_cb = onHostSync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    // 自定义 store：驱动不持久化密钥，仅内存缓存 + 上报上层（由上层决定如何存储/恢复）
    ble_hs_cfg.store_read_cb = storeReadCb;
    ble_hs_cfg.store_write_cb = storeWriteCb;
    ble_hs_cfg.store_delete_cb = storeDeleteCb;

    // 启用安全管理器（配对/绑定）。不配置则 security_initiate 不会生成/存储密钥，
    // storeWriteCb 也就不会触发，sigBondKeyReported 无从上报。
    ble_hs_cfg.sm_bonding = 1;                                   // 允许绑定，配对产物写入 store
    ble_hs_cfg.sm_mitm    = 0;                                   // Just Works，无需密码交互
    ble_hs_cfg.sm_sc      = 1;                                   // 安全连接（Secure Connections）
    ble_hs_cfg.sm_io_cap  = BLE_HS_IO_NO_INPUT_OUTPUT;           // 无输入输出 → Just Works
    ble_hs_cfg.sm_our_key_dist   = BLE_SM_PAIR_KEY_DIST_ENC;     // 本机分发加密密钥(LTK)
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC     // 对端分发 LTK（storeWriteCb 捕获）
                                  | BLE_SM_PAIR_KEY_DIST_ID;     // 以及 IRK（隐私地址解析）
    nimble_port_freertos_init([](void *) { nimble_port_run(); });
}
void JZBleDriverEsp32::onHostReset(int)
{
    // 复位时可做清理，这里保持简单
}
void JZBleDriverEsp32::onHostSync()
{
    auto *self = static_cast<JZBleDriverEsp32 *>(JZBleDriver::instance());
    if (!self) return;
    int rc = ble_hs_util_ensure_addr(0); // 优先公钥地址
    if (rc != 0) return;
    rc = ble_hs_id_infer_auto(0, &self->m_ownAddrType);
    if (rc != 0) self->m_ownAddrType = BLE_OWN_ADDR_PUBLIC;

    self->m_synced = true;

    // 注册 GATT 服务（必须在 host sync 之后、首次广播之前完成）
    if (self->m_periphGatt && !self->m_periphGatt->registered)
    {
        self->buildPeriphGatt();
        int rc2 = ble_gatts_count_cfg(self->m_periphGatt->svcDefs.data());
        if (rc2 == 0)
            rc2 = ble_gatts_add_svcs(self->m_periphGatt->svcDefs.data());
        self->m_periphGatt->registered = (rc2 == 0);
    }

    // 处理延迟广播请求
    if (self->m_pendingAdv && self->m_pendingAdvPeriph)
    {
        self->m_advertising = true;
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
                          onGapEvent, self->m_pendingAdvPeriph);

        self->m_pendingAdv = false;
        self->m_pendingAdvName.clear();
        self->m_pendingAdvUuids.clear();
        self->m_pendingAdvPeriph = nullptr;
    }
}
JZBleDriverEsp32::~JZBleDriverEsp32() { delete m_periphGatt; }
bool JZBleDriverEsp32::isBluetoothAvailable() { return true; }
bool JZBleDriverEsp32::isBluetoothEnabled()   { return true; }

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

    auto *self = static_cast<JZBleDriverEsp32 *>(JZBleDriver::instance());
    auto it = self->m_bondStore.find(bondStoreKey(obj_type, addr));
    if (it == self->m_bondStore.end()) return BLE_HS_ENOENT;

    bondKeyToSec(it->second, &dst->sec);
    return 0;
}

int JZBleDriverEsp32::storeWriteCb(int obj_type, const union ble_store_value *val)
{
    auto *self = static_cast<JZBleDriverEsp32 *>(JZBleDriver::instance());
    if (obj_type == BLE_STORE_OBJ_TYPE_PEER_SEC || obj_type == BLE_STORE_OBJ_TYPE_OUR_SEC) {
        std::string addr = addrToString(val->sec.peer_addr.val);
        JZBleBondKey key = secToBondKey(obj_type, &val->sec);
        self->m_bondStore[bondStoreKey(obj_type, addr)] = key;
        // 分别上报 PEER_SEC(对端密钥) 与 OUR_SEC(本机密钥)：两者都需持久化，
        // 外设角色重连加密依赖 OUR_SEC，故不跳过。
        if (self->m_bondCentral)
            JZBleDriver::centralOnBondKeyReported(self->m_bondCentral, addr, key);
        return 0;
    }
    return 0;   // 其他类型（CCCD 等）忽略，不持久化
}

int JZBleDriverEsp32::storeDeleteCb(int obj_type, const union ble_store_key *key)
{
    auto *self = static_cast<JZBleDriverEsp32 *>(JZBleDriver::instance());
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
void JZBleDriverEsp32::startScan()
{
    if (m_scanning) return;
    m_scanning = true;
    struct ble_gap_disc_params p; memset(&p, 0, sizeof(p)); p.passive = 1;
    ble_gap_disc(m_ownAddrType, BLE_HS_FOREVER, &p, onGapEvent, this);
}
void JZBleDriverEsp32::stopScan() { m_scanning = false; ble_gap_disc_cancel(); }
bool JZBleDriverEsp32::isScanning() { return m_scanning; }

void JZBleDriverEsp32::connectToDevice(JZBLECentral *d)
{
    m_centralSet.insert(d);
    m_bondCentral = d;   // 单一上报目标（NimBLE 静态回调无法指明具体对象，只支持一个）
    ble_addr_t peer;
    peer.type = BLE_ADDR_PUBLIC;
    addrFromString(d->deviceInfo().address(), peer.val);
    ble_gap_connect(m_ownAddrType, &peer, BLE_HS_FOREVER, nullptr, onGapEvent, d);
}
void JZBleDriverEsp32::disconnectFromDevice(JZBLECentral *d)
{
    if (m_bondCentral == d) m_bondCentral = nullptr;
    int h = getConnHandle(d->deviceInfo().address());
    if (h >= 0) ble_gap_terminate(h, BLE_ERR_REM_USER_CONN_TERM);
}
bool JZBleDriverEsp32::isConnected(JZBLECentral *d) { return getConnHandle(d->deviceInfo().address()) >= 0; }

void JZBleDriverEsp32::discoverServices(const std::string &addr, JZBLECentral *d)
{
    int h = getConnHandle(addr);
    if (h >= 0) ble_gattc_disc_all_svcs(h, onGattcSvcEvent, (void *)d);
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
void JZBleDriverEsp32::writeCharacteristic(const std::string &addr, const std::string &svc, const std::string &c, const std::string &v, bool nrs, JZBLEService *d)
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
void JZBleDriverEsp32::writeDescriptor(const std::string &addr, const std::string &svc, const std::string &c, const std::string &desc, const std::string &v, JZBLEService *d)
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

    std::map<std::string, std::vector<std::string>> svcChars;  // svcUuid -> [charUuids]
    for (auto &kv : m_periphCharMap)
        svcChars[kv.second.svcUuid].push_back(kv.first);

    for (auto &sc : svcChars)
    {
        ble_uuid_any_t svcUuid;
        uuidFromString(sc.first, &svcUuid);
        m_periphGatt->svcUuids.push_back(svcUuid);   // 存入长期存活的成员

        auto startIdx = m_periphGatt->chrDefs.size();
        for (auto &cuuid : sc.second)
        {
            ble_uuid_any_t cu;
            uuidFromString(cuuid, &cu);
            m_periphGatt->chrUuids.push_back(cu);

            int props = m_periphCharMap[cuuid].properties;
            ble_gatt_chr_flags nimFlags = 0;
            if (props & 0x01) nimFlags |= BLE_GATT_CHR_F_READ;
            if (props & 0x02) nimFlags |= BLE_GATT_CHR_F_WRITE;
            if (props & 0x04) nimFlags |= BLE_GATT_CHR_F_NOTIFY;
            if (props & 0x08) nimFlags |= BLE_GATT_CHR_F_INDICATE;
            if (props & 0x10) nimFlags |= BLE_GATT_CHR_F_WRITE_NO_RSP;

            ble_gatt_chr_def cd = {};
            cd.uuid = &m_periphGatt->chrUuids.back().u;
            cd.access_cb = onGattAccess;
            cd.val_handle = &m_periphCharMap[cuuid].valHandle; // NimBLE 自动分配并回写
            cd.flags = nimFlags;
            cd.min_key_size = 0;
            m_periphGatt->chrDefs.push_back(cd);
        }
        m_periphGatt->chrDefs.push_back(ble_gatt_chr_def{});  // 终止符

        ble_gatt_svc_def sd = {};
        sd.type = BLE_GATT_SVC_TYPE_PRIMARY;
        sd.uuid = &m_periphGatt->svcUuids.back().u;
        sd.characteristics = &m_periphGatt->chrDefs[startIdx];
        m_periphGatt->svcDefs.push_back(sd);
    }
    m_periphGatt->svcDefs.push_back(ble_gatt_svc_def{});  // 终止符
}

void JZBleDriverEsp32::startAdvertising(const std::string &name, const std::vector<JZBluetoothUuid> &uuids, JZBLEPeripheral *d)
{
    m_periphSet.insert(d);
    m_periphCtrlPriv = d;

    // host 尚未就绪：延迟到 sync_cb
    if (!m_synced) {
        m_pendingAdv = true;
        m_pendingAdvName = name;
        m_pendingAdvUuids = uuids;
        m_pendingAdvPeriph = d;
        return;
    }

    // 已就绪但 GATT 服务尚未注册
    if (!m_periphGatt->registered)
    {
        buildPeriphGatt();
        int rc = ble_gatts_count_cfg(m_periphGatt->svcDefs.data());
        if (rc == 0)
            rc = ble_gatts_add_svcs(m_periphGatt->svcDefs.data());
        m_periphGatt->registered = (rc == 0);
    }

    m_advertising = true;
    m_periphConnHandle = -1;

    struct ble_hs_adv_fields f; memset(&f, 0, sizeof(f));
    f.name = (uint8_t *)name.c_str(); f.name_len = name.size(); f.name_is_complete = 1;
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    std::vector<ble_uuid16_t> u16; u16.reserve(uuids.size());
    std::vector<ble_uuid128_t> u128; u128.reserve(uuids.size());
    for (auto &u : uuids) {
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
    ble_gap_adv_start(m_ownAddrType, nullptr, BLE_HS_FOREVER, &p, onGapEvent, d);
}

void JZBleDriverEsp32::stopAdvertising()
{
    m_advertising = false;
    m_periphConnHandle = -1;
    // 不在此清空 m_periphCtrlPriv / 不 erase m_periphSet：
    // 若旧连接仍存活，其后续 DISCONNECT 需靠集合判定来源，否则会误判为 Central 而崩溃。
    // 这些在 DISCONNECT peripheral 分支中清理。
    ble_gap_adv_stop();
}

void JZBleDriverEsp32::addPeripheralCharacteristic(const std::string &svcUuid, const std::string &charUuid, int properties)
{
    m_periphCharMap[charUuid] = { svcUuid, 0, properties, "" };
}

int JZBleDriverEsp32::sendPeriphData(int connHandle, uint16_t valHandle, const std::string &value, bool indicate)
{
    if (connHandle < 0 || valHandle == 0 || value.empty()) return -1;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(value.data(), (uint16_t)value.size());
    if (!om) return -1;
    int rc = indicate
        ? ble_gatts_indicate_custom(connHandle, valHandle, om)
        : ble_gatts_notify_custom(connHandle, valHandle, om);
    if (rc != 0) os_mbuf_free_chain(om);
    return rc;
}

// GATT access 回调：处理 Peripheral 端的特征读写
int JZBleDriverEsp32::onGattAccess(uint16_t conn_handle, uint16_t attr_handle,
                                    struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)arg;
    auto *self = static_cast<JZBleDriverEsp32 *>(JZBleDriver::instance());
    if (!self || !self->m_periphCtrlPriv) return BLE_ATT_ERR_UNLIKELY;

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR && ctxt->om)
    {
        for (auto &kv : self->m_periphCharMap)
        {
            if (kv.second.valHandle == attr_handle)
            {
                std::string val((const char *)ctxt->om->om_data, ctxt->om->om_len);
                kv.second.value = val;   // 保存以便后续读取
                JZBleDriver::peripheralOnCharWritten(self->m_periphCtrlPriv,
                                               kv.first.c_str(), val.c_str());
                return 0;
            }
        }
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
        return BLE_ATT_ERR_UNLIKELY;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

void JZBleDriverEsp32::peripheralSendNotify(const std::string &charUuid, const std::string &value)
{
    auto it = m_periphCharMap.find(charUuid);
    if (it == m_periphCharMap.end()) return;
    sendPeriphData(m_periphConnHandle, it->second.valHandle, value, false);
}

void JZBleDriverEsp32::peripheralSendIndicate(const std::string &charUuid, const std::string &value)
{
    auto it = m_periphCharMap.find(charUuid);
    if (it == m_periphCharMap.end()) return;
    sendPeriphData(m_periphConnHandle, it->second.valHandle, value, true);
}
