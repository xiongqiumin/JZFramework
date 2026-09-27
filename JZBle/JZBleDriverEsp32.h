#ifndef JZ_BLE_DRIVER_ESP32_H_
#define JZ_BLE_DRIVER_ESP32_H_
#include "JZBleDriver.h"
#include "JZBLEPeripheral.h"   // 完整定义 JZBLEPeripheralConnect（基类需要完整类型）
#include "JZByteArray.h"
#include "JZMutex.h"
#include <map>
#include <set>
#include <vector>
#include <string>
#include <memory>

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

// 从机特征定义（GATT 表为服务端共享；订阅状态改为按连接记录，见 JZBLEPeripheralConnectEsp32::subs）
struct PeriphCharInfo JZFRAMEWORK_API {
    std::string svcUuid;     // 所属 Service UUID 字符串
    uint16_t valHandle = 0;
    int properties = 0;
    JZByteArray value;
};

class JZFRAMEWORK_API JZBLEPeripheralConnectEsp32 : public JZBLEPeripheralConnect
{
public:
    virtual ~JZBLEPeripheralConnectEsp32()
    {
    }

    int periphConnHandle = -1;

    // 每个连接独立的订阅状态（特征 UUID 字符串 -> 订阅标记），多连接互不影响
    struct CharSub {
        bool subscribedNotify   = false;
        bool subscribedIndicate = false;
    };
    std::map<std::string, CharSub> subs;  // charUuid.toString() -> 订阅状态
};

class JZFRAMEWORK_API JZBleDriverEsp32 : public JZBleDriver
{
public:
    JZBleDriverEsp32();
    ~JZBleDriverEsp32() override;
    
    //主机
    void startScan(JZBLEDeviceDiscoveryAgent *agent) override;
    void stopScan(JZBLEDeviceDiscoveryAgent *agent) override;
    bool isScanning(JZBLEDeviceDiscoveryAgent *agent) override;

    void connectToDevice(JZBLEController *d, const std::vector<JZBleBondKey>& keys) override;
    void disconnectFromDevice(JZBLEController *d) override;
    bool isConnected(JZBLEController *d) override;

    void discoverServices(const std::string& address, JZBLEController *d) override;
    std::vector<JZBluetoothUuid> services(const std::string& address) override;

    void discoverCharacteristics(const std::string& address, const std::string& serviceUuid,
                                  JZBLEService *d) override;
    std::vector<JZLowEnergyCharacteristic> characteristics(const std::string& address,
                                                            const std::string& serviceUuid) override;

    void readCharacteristic(const std::string& address, const std::string& serviceUuid,
                             const std::string& charUuid, JZBLEService *d) override;
    void writeCharacteristic(const std::string& address, const std::string& serviceUuid,
                              const std::string& charUuid, const JZByteArray& value,
                              bool writeNoResponse, JZBLEService *d) override;
    void enableNotification(const std::string& address, const std::string& serviceUuid,
                             const std::string& charUuid, bool enable, JZBLEService *d) override;
    void readDescriptor(const std::string& address, const std::string& serviceUuid,
                         const std::string& charUuid, const std::string& descUuid,
                         JZBLEService *d) override;
    void writeDescriptor(const std::string& address, const std::string& serviceUuid,
                          const std::string& charUuid, const std::string& descUuid,
                          const JZByteArray& value, JZBLEService *d) override;
    
    //从机
    void startAdvertising(JZBLEPeripheral *d) override;
    void stopAdvertising(JZBLEPeripheral *d) override;
    void peripheralSendNotify(JZBLEPeripheral *d, JZBLEPeripheralConnect *connect, const JZBluetoothUuid& charUuid, const JZByteArray& value) override;
    void peripheralDisconnect(JZBLEPeripheral *d, JZBLEPeripheralConnect *connect);

    // 绑定密钥透传：Central 密钥经 connectToDevice(keys) 注入 / Peripheral 密钥经 setBondKey 预灌 / 主动配对
    void setBondKey(const std::string& addr, const JZBleDriver::JZBleBondKey& key) override;
    void requestPairing(const std::string& addr) override;

private:
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
        std::string svc, chr, desc;
        JZByteArray value;
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

    bool nimbleInit();
    void nimbleDeinit();
    void onAdvertisingStart();

    // ---- 工具 ----
    static std::string addrToString(const uint8_t *addr);
    static void       addrFromString(const std::string &str, uint8_t *addrOut);
    static int        uuidFromString(const std::string &str, void *uuidOut);
    static JZBluetoothUuid bleUuidToJZ(const ble_uuid_t *u);
    // 规范化比较：上层可能用短格式/大小写不同的 UUID 字符串
    static bool uuidEqual(const JZBluetoothUuid &a, const std::string &b);
    // 把任意格式 UUID 字符串规范化为 128-bit 小写形式（用于 map key）
    static std::string normalizeUuid(const std::string &s);
    int  getConnHandle(const std::string &address) const;
    void setConnHandle(const std::string &address, int handle);
    void removeConnHandle(const std::string &address);
    std::string connHandleToAddr(int handle) const;
    uint16_t findCharValHandle(const std::string &addr, const std::string &svc, const std::string &c);
    uint16_t findDscHandle(const std::string &addr, const std::string &svc, const std::string &c, const std::string &desc);
    void writeCccd(int connHandle, uint16_t cccdHandle, bool enable);
    void ensureDescriptorsDiscovered(const std::string &addr, const std::string &svc);
    void buildPeriphGatt();

    // GAP 事件回调：按角色拆分为三个独立回调，arg 类型由各注册点固定，
    // 因此回调内部无需运行时 cast 或角色判断，直接按固定类型使用 arg。
    static int onScanGapEvent(struct ble_gap_event *event, void *arg);    // arg = driver(this)，扫描
    static int onPeriphGapEvent(struct ble_gap_event *event, void *arg); // arg = JZBLEPeripheral*
    static int onCentralGapEvent(struct ble_gap_event *event, void *arg);// arg = JZBLEController*
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
    static int sendPeriphData(int connHandle, uint16_t valHandle, const JZByteArray &value, bool indicate);

    // ---- 绑定密钥 store 回调（驱动不持久化，仅内存缓存 + 上报上层）----
    static int storeReadCb(int obj_type, const union ble_store_key *key, union ble_store_value *dst);
    static int storeWriteCb(int obj_type, const union ble_store_value *val);
    static int storeDeleteCb(int obj_type, const union ble_store_key *key);
    static JZBleBondKey secToBondKey(int obj_type, const struct ble_store_value_sec *sec);
    static void bondKeyToSec(const JZBleBondKey &key, struct ble_store_value_sec *sec);

    // ---- 共享 ----
    mutable JZMutex m_mutex;   // 保护所有跨线程共享状态（应用线程 <-> NimBLE 任务线程）
    bool m_bleInit = false;
    uint8_t m_ownAddrType = 0;  // 0 = BLE_OWN_ADDR_PUBLIC
    std::map<std::string, JZBleBondKey> m_bondStore; // obj_type:addr -> 密钥

    // ---- Peripheral (从机) ----
    enum class State { None, Starting, Started };
    State m_advertState = State::None;
    JZBLEPeripheral *m_periphCtrlPriv = nullptr;
    PeriphGatt *m_periphGatt = nullptr;  // 完整定义见 .cpp（含 NimBLE 类型，头文件仅前置声明）
    std::map<std::string, PeriphCharInfo> m_periphCharMap; // charUuid -> info
    std::map<int, std::shared_ptr<JZBLEPeripheralConnectEsp32>> m_periphConnMap; // conn_handle -> 连接
    std::string m_pendingAdvName;
    std::vector<JZBluetoothUuid> m_pendingAdvUuids;

    // ---- Central (主机) ----
    bool m_scanning = false;
    std::map<std::string, int> m_connMap;
    std::map<int, std::string> m_connToAddr;
    std::map<std::string, ConnGatt> m_gatt;
    std::map<std::string, JZBLEService*> m_svcPtrCache;    // "addr|svc" -> service
    std::map<std::string, JZBLEController *> m_bondCentralByAddr;
};

#endif // !JZ_BLE_DRIVER_ESP32_H_
