#include "JZBleDriverEsp32.h"
#include "JZBLEController.h"
#include "JZBLEService.h"
#include "JZBLEDeviceDiscoveryAgent.h"
#include "JZStd.h"

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

// ==================== 工具 ====================
std::string JZBleDriverEsp32::addrToString(const uint8_t *addr)
{
    char buf[18]; snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x",
        addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
    return std::string(buf);
}
void JZBleDriverEsp32::addrFromString(const std::string &str, uint8_t *addrOut)
{
    for (int i = 0, pos = 0; i < 6; i++, pos += 3) {
        uint8_t hi = (uint8_t)(str[pos] > '9' ? str[pos] - 'a' + 10 : str[pos] - '0');
        uint8_t lo = (uint8_t)(str[pos + 1] > '9' ? str[pos + 1] - 'a' + 10 : str[pos + 1] - '0');
        addrOut[5 - i] = (hi << 4) | lo;
    }
}
int JZBleDriverEsp32::uuidFromString(const std::string &str, void *uuidOut)
{
    ble_uuid_any_t *u = (ble_uuid_any_t *)uuidOut;
    if (str_startsWith(str, "0000") && str_endsWith(str, "-0000-1000-8000-00805f9b34fb") && str.size() >= 8)
        { u->u.type = BLE_UUID_TYPE_16; u->u16.value = (uint16_t)strtol(str.substr(4, 4).c_str(), nullptr, 16); return 0; }
    u->u.type = BLE_UUID_TYPE_128;
    return ble_uuid_parse_from_str(str.c_str(), &u->u) == 0 ? 0 : -1;
}

int  JZBleDriverEsp32::getConnHandle(const std::string &a) { auto it = m_connMap.find(a); return it != m_connMap.end() ? it->second : -1; }
void JZBleDriverEsp32::setConnHandle(const std::string &a, int h) { m_connMap[a] = h; }
void JZBleDriverEsp32::removeConnHandle(const std::string &a) { m_connMap.erase(a); }

// ==================== GAP 回调 — arg 透传自 NimBLE 调用 ====================
int JZBleDriverEsp32::onGapEvent(struct ble_gap_event *event, void *arg)
{
    auto *self = (JZBleDriverEsp32 *)arg;

    switch (event->type)
    {
    case BLE_GAP_EVENT_DISC:   // arg = this（扫描），多播给所有 Controller & Agent
    {
        JZBluetoothDeviceInfo info;
        info.setAddress(addrToString(event->disc.addr.val));
        info.setRssi(event->disc.rssi);
        struct ble_hs_adv_fields f;
        if (ble_hs_adv_parse_fields(&f, event->disc.data, event->disc.length_data) == 0 && f.name && f.name_len > 0)
            info.setName(std::string((const char *)f.name, f.name_len));
        for (auto *d : self->m_controllers)
            JZBLEControllerPrivate::onDeviceDiscovered(d, info);
        for (auto *d : self->m_agents)
            JZBLEDeviceDiscoveryAgentPrivate::onDeviceDiscovered(d, info);
        break;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
        self->m_scanning = false;
        for (auto *d : self->m_controllers)
            JZBLEControllerPrivate::onScanFinished(d);
        for (auto *d : self->m_agents)
            JZBLEDeviceDiscoveryAgentPrivate::onScanFinished(d);
        break;

    case BLE_GAP_EVENT_CONNECT:  // arg 可能是 ctrlD (ble_gap_connect) 或 phrD (ble_gap_adv_start)
    {
        struct ble_gap_conn_desc desc;
        ble_gap_conn_find(event->connect.conn_handle, &desc);
        std::string addr = addrToString(desc.peer_id_addr.val);

        if (self->m_advertising)
        {
            if (event->connect.status == 0)
            {
                self->m_periphConnHandle = event->connect.conn_handle;
                JZBLEControllerPrivate::onClientConnected((JZBLEControllerPrivate *)arg);
            }
        }
        else
        {
            auto *d = (JZBLEControllerPrivate *)arg;
            if (event->connect.status == 0)
            {
                self->setConnHandle(addr, event->connect.conn_handle);
                JZBLEControllerPrivate::onDeviceConnected(d, addr.c_str());
            }
            else
                JZBLEControllerPrivate::onError(d, 0, "GAP connect failed");
        }
        break;
    }
    case BLE_GAP_EVENT_DISCONNECT:
    {
        std::string addr = addrToString(event->disconnect.conn.peer_id_addr.val);
        if (self->m_advertising)
        {
            self->m_periphConnHandle = -1;
            JZBLEControllerPrivate::onClientDisconnected((JZBLEControllerPrivate *)arg);
        }
        else
        {
            self->removeConnHandle(addr);
            if (self->m_disconnectCtrl)
            {
                JZBLEControllerPrivate::onDeviceDisconnected(self->m_disconnectCtrl, addr.c_str());
                self->m_disconnectCtrl = nullptr;
            }
        }
        break;
    }
    case BLE_GAP_EVENT_NOTIFY_RX:
    {
        std::string v((const char *)event->notify_rx.om->om_data, event->notify_rx.om->om_len);
        JZBLEServicePrivate::onCharacteristicChanged((JZBLEServicePrivate *)arg, "", "", v.c_str());
        break;
    }
    case BLE_GAP_EVENT_SUBSCRIBE:
    case BLE_GAP_EVENT_MTU:
        break;
    }
    return 0;
}

// ==================== GATT 事件回调 ====================
static int onGattcSvcEvent(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_error *error,
                            union ble_gatt_event_data *eventData, void *arg)
{
    if (error->status == 0 || error->status == BLE_HS_EDONE)
        JZBLEControllerPrivate::onServicesDiscovered((JZBLEControllerPrivate *)arg, "");
    return 0;
}

static int onGattcCharEvent(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_error *error,
                             union ble_gatt_event_data *eventData, void *arg)
{
    if (error->status == 0 || error->status == BLE_HS_EDONE)
        JZBLEServicePrivate::onCharacteristicsDiscovered((JZBLEServicePrivate *)arg, "", "");
    return 0;
}

static int onGattcRWEvent(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_error *error,
                           union ble_gatt_event_data *eventData, void *arg)
{
    (void)arg;
    if (error->status != 0 && error->status != BLE_HS_EDONE) return 0;
    return 0;
}

// ==================== 构造/析构 ====================
JZBleDriverEsp32::JZBleDriverEsp32() : m_scanning(false), m_advertising(false), m_disconnectCtrl(nullptr)
{
    nvs_flash_init();
    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    esp_bt_controller_config_t c = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_bt_controller_init(&c); esp_bt_controller_enable(ESP_BT_MODE_BLE);
    esp_bluedroid_init(); esp_bluedroid_enable();
    nimble_port_init();
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_gap_event_register(onGapEvent, this);
    nimble_port_freertos_init([](void *) { nimble_port_run(); });
}
JZBleDriverEsp32::~JZBleDriverEsp32() {}
bool JZBleDriverEsp32::isBluetoothAvailable() { return true; }
bool JZBleDriverEsp32::isBluetoothEnabled()   { return true; }

// ==================== Central ====================
void JZBleDriverEsp32::startScan()
{
    if (m_scanning) return; m_scanning = true;
    struct ble_gap_disc_params p; memset(&p, 0, sizeof(p)); p.passive = 1;
    ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &p, onGapEvent, this);
}
void JZBleDriverEsp32::stopScan() { m_scanning = false; ble_gap_disc_cancel(); }
bool JZBleDriverEsp32::isScanning() { return m_scanning; }

void JZBleDriverEsp32::connectToDevice(const std::string &addr, JZBLEControllerPrivate *d)
{
    ble_addr_t peer; peer.type = BLE_ADDR_PUBLIC; addrFromString(addr, peer.val);
    ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &peer, BLE_HS_FOREVER, nullptr, onGapEvent, d);
}
void JZBleDriverEsp32::disconnectFromDevice(const std::string &addr, JZBLEControllerPrivate *d)
{
    m_disconnectCtrl = d;
    int h = getConnHandle(addr);
    if (h >= 0) ble_gap_terminate(h, BLE_ERR_REM_USER_CONN_TERM);
}
bool JZBleDriverEsp32::isConnected(const std::string &addr) { return getConnHandle(addr) >= 0; }

void JZBleDriverEsp32::discoverServices(const std::string &addr, JZBLEControllerPrivate *d)
{
    int h = getConnHandle(addr);
    if (h >= 0) ble_gattc_disc_all_svcs(h, onGattcSvcEvent, d);
}
std::vector<JZBluetoothUuid> JZBleDriverEsp32::services(const std::string &addr)
    { auto it = m_svcCache.find(addr); return it != m_svcCache.end() ? it->second : std::vector<JZBluetoothUuid>(); }

void JZBleDriverEsp32::discoverCharacteristics(const std::string &addr, const std::string &svc, JZBLEServicePrivate *d)
{
    int h = getConnHandle(addr); if (h < 0) return;
    ble_uuid_any_t u; uuidFromString(svc, &u);
    ble_gattc_disc_chrs_by_uuid(h, 0x0001, 0xFFFF, &u.u, onGattcCharEvent, d);
}
std::vector<JZLowEnergyCharacteristic> JZBleDriverEsp32::characteristics(const std::string &addr, const std::string &svc)
    { auto it = m_charCache.find(addr + "|" + svc); return it != m_charCache.end() ? it->second : std::vector<JZLowEnergyCharacteristic>(); }

void JZBleDriverEsp32::readCharacteristic(const std::string &addr, const std::string &svc, const std::string &c, JZBLEServicePrivate *d)
    { int h = getConnHandle(addr); if (h < 0) return; ble_uuid_any_t u; if (uuidFromString(c, &u) != 0) return; ble_gattc_read_by_uuid(h, 0x0001, 0xFFFF, &u.u, onGattcRWEvent, d); }
void JZBleDriverEsp32::writeCharacteristic(const std::string &addr, const std::string &svc, const std::string &c, const std::string &v, bool nrs, JZBLEServicePrivate *d)
    { int h = getConnHandle(addr); if (h < 0) return; if (nrs) ble_gattc_write_no_rsp_flat(h, 0, v.data(), v.size()); else ble_gattc_write_flat(h, 0, v.data(), v.size(), onGattcRWEvent, d); }
void JZBleDriverEsp32::enableNotification(const std::string &addr, const std::string &svc, const std::string &c, bool en, JZBLEServicePrivate *d)
    { int h = getConnHandle(addr); if (h < 0) return; uint16_t v = en ? 0x0001 : 0x0000; ble_gattc_write_flat(h, 0, &v, sizeof(v), onGattcRWEvent, d); }
void JZBleDriverEsp32::readDescriptor(const std::string &addr, const std::string &svc, const std::string &c, const std::string &desc, JZBLEServicePrivate *d)
    { int h = getConnHandle(addr); if (h < 0) return; ble_uuid_any_t u; if (uuidFromString(desc, &u) != 0) return; ble_gattc_read_by_uuid(h, 0x0001, 0xFFFF, &u.u, onGattcRWEvent, d); }
void JZBleDriverEsp32::writeDescriptor(const std::string &addr, const std::string &svc, const std::string &c, const std::string &desc, const std::string &v, JZBLEServicePrivate *d)
    { int h = getConnHandle(addr); if (h < 0) return; ble_gattc_write_flat(h, 0, v.data(), v.size(), onGattcRWEvent, d); }

// ==================== Peripheral ====================
void JZBleDriverEsp32::startAdvertising(const std::string &name, const std::vector<JZBluetoothUuid> &uuids, JZBLEControllerPrivate *d)
{
    m_advertising = true;
    m_periphConnHandle = -1;
    m_periphCtrlPriv = d;

    // 如果有已注册特征，构建并注册 GATT 服务
    if (!m_periphCharMap.empty())
    {
        std::map<std::string, std::vector<std::string>> svcChars;  // svcUuid → [charUuids]
        for (auto &kv : m_periphCharMap)
            svcChars[kv.second.svcUuid].push_back(kv.first);

        std::vector<ble_gatt_chr_def> chrDefs;
        std::vector<ble_uuid_any_t>   chrUuids;
        std::vector<ble_gatt_svc_def> svcDefs;
        chrDefs.reserve(m_periphCharMap.size() + svcChars.size());
        chrUuids.reserve(m_periphCharMap.size());
        svcDefs.reserve(svcChars.size() + 1);

        for (auto &sc : svcChars)
        {
            ble_uuid_any_t svcUuid128;
            uuidFromString(sc.first, &svcUuid128);

            auto startIdx = chrDefs.size();
            for (auto &cuuid : sc.second)
            {
                ble_uuid_any_t cu;
                uuidFromString(cuuid, &cu);
                chrUuids.push_back(cu);

                // 从 JZLowEnergyCharacteristicProperty 转换为 NimBLE flags
                int props = m_periphCharMap[cuuid].properties;
                ble_gatt_chr_flags nimFlags = 0;
                if (props & 0x01) nimFlags |= BLE_GATT_CHR_F_READ;
                if (props & 0x02) nimFlags |= BLE_GATT_CHR_F_WRITE;
                if (props & 0x04) nimFlags |= BLE_GATT_CHR_F_NOTIFY;
                if (props & 0x08) nimFlags |= BLE_GATT_CHR_F_INDICATE;
                if (props & 0x10) nimFlags |= BLE_GATT_CHR_F_WRITE_NO_RSP;

                ble_gatt_chr_def cd = {};
                cd.uuid = &chrUuids.back().u;
                cd.access_cb = onGattAccess;
                cd.val_handle = nullptr;  // 自动分配
                cd.flags = nimFlags;
                cd.min_key_size = 0;
                chrDefs.push_back(cd);
            }

            ble_gatt_chr_def term = {0};  // 终止符
            chrDefs.push_back(term);

            ble_gatt_svc_def sd = {};
            sd.type = BLE_GATT_SVC_TYPE_PRIMARY;
            sd.uuid = &svcUuid128.u;
            sd.characteristics = &chrDefs[startIdx];
            svcDefs.push_back(sd);
        }

        ble_gatt_svc_def termSvc = {0};  // 终止符
        svcDefs.push_back(termSvc);

        int rc = ble_gatts_count_cfg(svcDefs.data());
        if (rc == 0)
            rc = ble_gatts_add_svcs(svcDefs.data());
        if (rc == 0)
        {
            // 注册成功后查找每个特征的 value handle
            for (auto &kv : m_periphCharMap)
            {
                ble_uuid_any_t su, cu;
                if (uuidFromString(kv.second.svcUuid, &su) == 0 && uuidFromString(kv.first, &cu) == 0)
                {
                    uint16_t val_handle = 0;
                    int frc = ble_gatts_find_chr(&su.u, &cu.u, nullptr, &val_handle);
                    if (frc == 0 && val_handle != 0)
                        kv.second.valHandle = val_handle;
                }
            }
        }
    }

    struct ble_gap_adv_params p; memset(&p, 0, sizeof(p));
    p.conn_mode = BLE_GAP_CONN_MODE_UND; p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    struct ble_hs_adv_fields f; memset(&f, 0, sizeof(f));
    f.name = (uint8_t *)name.c_str(); f.name_len = name.size(); f.name_is_complete = 1;
    ble_gap_adv_set_fields(&f);
    ble_gap_adv_start(BLE_OWN_ADDR_RANDOM, nullptr, BLE_HS_FOREVER, &p, onGapEvent, d);
}

void JZBleDriverEsp32::stopAdvertising()
{
    m_advertising = false;
    m_periphConnHandle = -1;
    m_periphCtrlPriv = nullptr;
    ble_gap_adv_stop();
}

void JZBleDriverEsp32::addPeripheralCharacteristic(const std::string &svcUuid, const std::string &charUuid, int properties)
{
    m_periphCharMap[charUuid] = {svcUuid, 0, properties};
}

int sendPeriphData(int connHandle, uint16_t valHandle, const std::string &value, bool indicate)
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
    auto *self = static_cast<JZBleDriverEsp32 *>(JZBleAbstractDriver::instance());
    if (!self || !self->m_periphCtrlPriv) return BLE_ATT_ERR_UNLIKELY;

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR && ctxt->om)
    {
        // 查找 attr_handle 对应的 charUuid
        for (auto &kv : self->m_periphCharMap)
        {
            if (kv.second.valHandle == attr_handle)
            {
                std::string val((const char *)ctxt->om->om_data, ctxt->om->om_len);
                JZBLEControllerPrivate::onCharWritten(self->m_periphCtrlPriv,
                                                      kv.first.c_str(), val.c_str());
                break;
            }
        }
    }
    return 0;
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
