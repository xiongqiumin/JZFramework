#ifndef JZ_BLE_DRIVER_H_
#define JZ_BLE_DRIVER_H_

#include <string>
#include <vector>
#include "JZStd.h"
#include "JZBluetoothDeviceInfo.h"
#include "JZBluetoothUuid.h"
#include "JZLowEnergyCharacteristic.h"
#include "JZLowEnergyDescriptor.h"

class JZBLEPeripheral;
class JZBLECentral;
class JZBLEService;
class JZBLEDeviceDiscoveryAgentPrivate;

class JZBleDriver
{
public:
    JZBleDriver() = default;
    virtual ~JZBleDriver() = default;

    static JZBleDriver *instance();
    static void setInstance(JZBleDriver *inst);

    virtual bool isBluetoothAvailable() = 0;
    virtual bool isBluetoothEnabled() = 0;

    // === Central 扫描结果多播给 Discovery Agent ===
    void registerAgent(JZBLEDeviceDiscoveryAgentPrivate *d);
    void unregisterAgent(JZBLEDeviceDiscoveryAgentPrivate *d);

    virtual void startScan() = 0;
    virtual void stopScan() = 0;
    virtual bool isScanning() = 0;

    virtual void connectToDevice(JZBLECentral *d) = 0;
    virtual void disconnectFromDevice(JZBLECentral *d) = 0;
    virtual bool isConnected(JZBLECentral *d) = 0;

    virtual void discoverServices(const std::string& address, JZBLECentral *d) = 0;
    virtual std::vector<JZBluetoothUuid> services(const std::string& address) = 0;

    virtual void discoverCharacteristics(const std::string& address, const std::string& serviceUuid,
                                          JZBLEService *d) = 0;
    virtual std::vector<JZLowEnergyCharacteristic> characteristics(
        const std::string& address, const std::string& serviceUuid) = 0;

    virtual void readCharacteristic(const std::string& address, const std::string& serviceUuid,
                                     const std::string& charUuid, JZBLEService *d) = 0;
    virtual void writeCharacteristic(const std::string& address, const std::string& serviceUuid,
                                      const std::string& charUuid, const std::string& value,
                                      bool writeNoResponse, JZBLEService *d) = 0;
    virtual void enableNotification(const std::string& address, const std::string& serviceUuid,
                                     const std::string& charUuid, bool enable, JZBLEService *d) = 0;
    virtual void readDescriptor(const std::string& address, const std::string& serviceUuid,
                                 const std::string& charUuid, const std::string& descUuid,
                                 JZBLEService *d) = 0;
    virtual void writeDescriptor(const std::string& address, const std::string& serviceUuid,
                                  const std::string& charUuid, const std::string& descUuid,
                                  const std::string& value, JZBLEService *d) = 0;

    // === Peripheral ===
    virtual void addPeripheralCharacteristic(const std::string& svcUuid, const std::string& charUuid, int properties) {}
    virtual void startAdvertising(const std::string& name, const std::vector<JZBluetoothUuid>& serviceUuids,
                                   JZBLEPeripheral *d) = 0;
    virtual void stopAdvertising() = 0;
    virtual void peripheralSendNotify(const std::string& charUuid, const std::string& value) = 0;
    virtual void peripheralSendIndicate(const std::string& charUuid, const std::string& value) = 0;

    // === 绑定密钥透传（驱动不持久化，改由上层负责存储/恢复）===
    // 配对完成后密钥通过 sigBondKeyReported 信号上报（跨线程经 runOnObject 切到 Central 线程）；
    // 连接(重连)前由上层用 setBondKey 回灌，驱动据此在内存中恢复加密会话。
    // 驱动自身不做任何 Flash/文件持久化。
    struct JZBleBondKey {
        std::string peerAddr;        // "xx:xx:xx:xx:xx:xx"
        uint8_t     peerAddrType = 0;// BLE_ADDR_PUBLIC / BLE_ADDR_RANDOM
        uint8_t     objType = 0;     // BLE_STORE_OBJ_TYPE_PEER_SEC / OUR_SEC，回灌时据此定位
        uint8_t     keySize = 0;
        uint16_t    ediv = 0;
        uint64_t    randNum = 0;
        uint8_t     ltk[16] = {0};
        bool        ltkPresent = false;
        uint8_t     irk[16] = {0};
        bool        irkPresent = false;
        uint8_t     csrk[16] = {0};
        bool        csrkPresent = false;
        bool        authenticated = false;
        bool        secureConnections = false;
    };

    // 连接/重连前由上层把之前上报的密钥回灌，驱动据此恢复加密会话
    virtual void setBondKey(const std::string& addr, const JZBleBondKey& key) { (void)addr; (void)key; }
    // 主动发起配对（配对完成后密钥通过 sigBondKeyReported 信号上报）
    virtual void requestPairing(const std::string& addr) { (void)addr; }

    // === 事件转发中转 (JZBLECentral / JZBLEPeripheral 的 friend) ===
    // 具体驱动不得直接调用 JZBLECentral / JZBLEPeripheral 的受保护静态分发方法,
    // 统一经由此处转发, 避免在子类中暴露 friend 关系。
    static void centralOnDeviceConnected(JZBLECentral *self, const char *addr);
    static void centralOnDeviceDisconnected(JZBLECentral *self, const char *addr);
    static void centralOnServicesDiscovered(JZBLECentral *self, const char *addr);
    static void centralOnError(JZBLECentral *self, int code, const char *msg);
    // 配对完成上报：驱动在 NimBLE 线程捕获密钥后，经此中转切到 Central 线程 emit 信号
    static void centralOnBondKeyReported(JZBLECentral *self, const std::string &addr, const JZBleBondKey &key);

    static void peripheralOnClientConnected(JZBLEPeripheral *self);
    static void peripheralOnClientDisconnected(JZBLEPeripheral *self);
    static void peripheralOnCharWritten(JZBLEPeripheral *self, const char *charUuid, const char *value);

protected:
    JZ_DISABLE_COPY(JZBleDriver)

    // 扫描结果多播给 Discovery Agent
    std::vector<JZBLEDeviceDiscoveryAgentPrivate*> m_agents;
    // 绑定密钥上报目标。NimBLE 的 store 回调是静态的，无法指明具体 Central 对象，
    // 故只支持单个（最近一次 connect 的）Central，不做多对象路由。
    JZBLECentral *m_bondCentral = nullptr;
    static JZBleDriver *s_instance;
};

#endif // !JZ_BLE_DRIVER_H_
