#ifndef JZ_BLE_DRIVER_ESP32_H_
#define JZ_BLE_DRIVER_ESP32_H_

#include "JZBleDriver.h"
#include <map>
#include <set>
#include <vector>
#include <string>

// 前置声明：避免在头文件引入 NimBLE 头文件。NimBLE 类型需要 syscfg 等
// 配置，只能在 .cpp 中（已包含 nimble_port 之后）使用完整定义。
// 例外：ble_uuid.h 仅含基础 UUID 类型，不依赖 syscfg，此处引入以声明
// ble_uuid_t（bleUuidToJZ 的参数类型）。
#include "host/ble_uuid.h"
struct ble_gap_event;
struct ble_gatt_access_ctxt;
struct ble_gatt_error;
struct ble_gatt_svc;
struct ble_gatt_chr;
struct ble_gatt_attr;
struct ble_gatt_dsc;
struct PeriphGatt;

class JZBleDriverEsp32 : public JZBleDriver
{
public:
    JZBleDriverEsp32();
    ~JZBleDriverEsp32() override;

    bool isBluetoothAvailable() override;
    bool isBluetoothEnabled() override;

    void startScan() override;
    void stopScan() override;
    bool isScanning() override;

    void connectToDevice(JZBLECentral *d) override;
    void disconnectFromDevice(JZBLECentral *d) override;
    bool isConnected(JZBLECentral *d) override;

    void discoverServices(const std::string& address, JZBLECentral *d) override;
    std::vector<JZBluetoothUuid> services(const std::string& address) override;

    void discoverCharacteristics(const std::string& address, const std::string& serviceUuid,
                                  JZBLEService *d) override;
    std::vector<JZLowEnergyCharacteristic> characteristics(const std::string& address,
                                                            const std::string& serviceUuid) override;

    void readCharacteristic(const std::string& address, const std::string& serviceUuid,
                             const std::string& charUuid, JZBLEService *d) override;
    void writeCharacteristic(const std::string& address, const std::string& serviceUuid,
                              const std::string& charUuid, const std::string& value,
                              bool writeNoResponse, JZBLEService *d) override;
    void enableNotification(const std::string& address, const std::string& serviceUuid,
                             const std::string& charUuid, bool enable, JZBLEService *d) override;
    void readDescriptor(const std::string& address, const std::string& serviceUuid,
                         const std::string& charUuid, const std::string& descUuid,
                         JZBLEService *d) override;
    void writeDescriptor(const std::string& address, const std::string& serviceUuid,
                          const std::string& charUuid, const std::string& descUuid,
                          const std::string& value, JZBLEService *d) override;

    void startAdvertising(const std::string& name, const std::vector<JZBluetoothUuid>& serviceUuids,
                           JZBLEPeripheral *d) override;
    void stopAdvertising() override;
    void peripheralSendNotify(const std::string& charUuid, const std::string& value) override;
    void peripheralSendIndicate(const std::string& charUuid, const std::string& value) override;

    void addPeripheralCharacteristic(const std::string& svcUuid, const std::string& charUuid, int properties) override;

private:
    // ---- 工具 ----
    static std::string addrToString(const uint8_t *addr);
    static void       addrFromString(const std::string &str, uint8_t *addrOut);
    static int        uuidFromString(const std::string &str, void *uuidOut);
    static JZBluetoothUuid bleUuidToJZ(const ble_uuid_t *u);
    // 规范化比较：上层可能用短格式/大小写不同的 UUID 字符串
    static bool uuidEqual(const JZBluetoothUuid &a, const std::string &b);
    // 把任意格式 UUID 字符串规范化为 128-bit 小写形式（用于 map key）
    static std::string normalizeUuid(const std::string &s);
    int  getConnHandle(const std::string &address);
    void setConnHandle(const std::string &address, int handle);
    void removeConnHandle(const std::string &address);
    std::string connHandleToAddr(int handle) const;
    uint16_t findCharValHandle(const std::string &addr, const std::string &svc, const std::string &c);
    uint16_t findDscHandle(const std::string &addr, const std::string &svc, const std::string &c, const std::string &desc);
    void writeCccd(int connHandle, uint16_t cccdHandle, bool enable);
    void ensureDescriptorsDiscovered(const std::string &addr, const std::string &svc);
    void buildPeriphGatt();

    static int onGapEvent(struct ble_gap_event *event, void *arg);
    static void onHostSync();
    static void onHostReset(int reason);
    static int onGattAccess(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt, void *arg);

    // GATT 客户端（Central）事件回调（静态成员，可访问私有成员）
    static int onGattcSvcEvent(uint16_t conn_handle, const struct ble_gatt_error *error,
                               const struct ble_gatt_svc *svc, void *arg);
    static int onGattcCharEvent(uint16_t conn_handle, const struct ble_gatt_error *error,
                                const struct ble_gatt_chr *chr, void *arg);
    static int onGattcReadEvent(uint16_t conn_handle, const struct ble_gatt_error *error,
                                struct ble_gatt_attr *attr, void *arg);
    static int onGattcWriteEvent(uint16_t conn_handle, const struct ble_gatt_error *error,
                                 struct ble_gatt_attr *attr, void *arg);
    static int onGattcDscEvent(uint16_t conn_handle, const struct ble_gatt_error *error,
                                uint16_t attr_handle, const struct ble_gatt_dsc *dsc, void *arg);
    static int onGattcReadDscEvent(uint16_t conn_handle, const struct ble_gatt_error *error,
                                   struct ble_gatt_attr *attr, void *arg);
    static int onGattcWriteDscEvent(uint16_t conn_handle, const struct ble_gatt_error *error,
                                    struct ble_gatt_attr *attr, void *arg);
    static int onGattcAck(uint16_t conn_handle, const struct ble_gatt_error *error,
                            struct ble_gatt_attr *attr, void *arg);
    static int sendPeriphData(int connHandle, uint16_t valHandle, const std::string &value, bool indicate);

    // ---- Central GATT 缓存 ----
    struct GattCharInfo {
        JZBluetoothUuid uuid;
        uint16_t valHandle = 0;
        int properties = 0;
    };
    struct GattSvcInfo {
        JZBluetoothUuid uuid;
        uint16_t startHandle = 0, endHandle = 0;
    };
    struct GattDscInfo {
        JZBluetoothUuid uuid;
        uint16_t handle = 0;
    };
    struct PendingDsc {
        enum Type { Notify, Read, Write } type;
        std::string svc, chr, desc, value;
        bool enable = false;
        JZBLEService *d = nullptr;
    };
    struct ConnGatt {
        std::vector<GattSvcInfo> svcs;
        std::map<std::string, std::vector<GattCharInfo>> chars;        // svcStr -> chars
        std::map<std::string, std::vector<GattDscInfo>> dscs;         // "svc|char" -> dscs
        std::map<uint16_t, std::pair<std::string,std::string>> handleMap; // valHandle -> (svc, char)
        std::map<std::string, uint16_t> cccd;                        // "svc|char" -> cccd handle
        bool dscDiscovering = false;
        std::vector<PendingDsc> pendingDsc;
    };

    // ---- Peripheral GATT 特征（值用于读回）----
    struct PeriphCharInfo {
        std::string svcUuid;
        uint16_t valHandle = 0;
        int properties = 0;
        std::string value;
    };

    bool m_scanning = false;
    bool m_advertising = false;
    uint8_t m_ownAddrType = 0;  // 0 = BLE_OWN_ADDR_PUBLIC

    int m_periphConnHandle = -1;  // Peripheral client 连接句柄
    std::map<std::string, PeriphCharInfo> m_periphCharMap;  // charUuid -> handles/props/value
    JZBLEPeripheral *m_periphCtrlPriv = nullptr;    // 当前 Peripheral
    PeriphGatt *m_periphGatt = nullptr;              // 长期存活的 GATT 定义

    // 活跃实例集合：用于在 GAP 事件中区分事件来源（避免 stopAdvertising 清空
    // m_periphCtrlPriv 后，旧连接的 DISCONNECT 被误判为 Central 而崩溃）
    std::set<JZBLEPeripheral *> m_periphSet;
    std::set<JZBLECentral *>     m_centralSet;

    // 延迟广播（等待 host sync）
    bool m_synced = false;
    bool m_pendingAdv = false;
    std::string m_pendingAdvName;
    std::vector<JZBluetoothUuid> m_pendingAdvUuids;
    JZBLEPeripheral *m_pendingAdvPeriph = nullptr;

    std::map<std::string, int> m_connMap;
    std::map<int, std::string> m_connToAddr;                 // conn_handle -> addr
    std::map<std::string, ConnGatt> m_gatt;                 // addr -> GATT 缓存
    std::map<std::string, JZBLEService*> m_svcPtrCache; // "addr|svc" -> service 指针（通知路由用）
};

#endif // !JZ_BLE_DRIVER_ESP32_H_
