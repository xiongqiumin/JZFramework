#ifndef JZ_BLE_DRIVER_H_
#define JZ_BLE_DRIVER_H_

#include <string>
#include <vector>
#include "JZStd.h"
#include "JZObjectPointer.h"
#include "JZBLEDeviceInfo.h"
#include "JZBluetoothUuid.h"
#include "JZLowEnergyCharacteristic.h"
#include "JZLowEnergyDescriptor.h"
#include "JZByteArray.h"

class JZBLEPeripheral;
class JZBLEController;
class JZBLEService;
class JZBLEDeviceDiscoveryAgent;

class JZBleDriver
{
public:
    JZBleDriver() = default;
    virtual ~JZBleDriver() = default;

    static JZBleDriver *instance();
    static void setInstance(JZBleDriver *inst);

    // === 绑定密钥结构体（必须在 connectToDevice 声明之前定义）===
    // 配对完成后密钥通过 sigBondKeyReported 信号上报（跨线程经 runOnObject 切到 Central 线程）；
    // Central 重连加密所需的密钥由上层在调用 connectToDevice(keys) 时一并传入；
    // Peripheral 重连加密所需的密钥由上层在连接建立前通过 setBondKey 预灌。
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
    
    // === Central 扫描（参考 JZWifiDriverEsp32，由发起扫描的 Agent 自行传入自身）===
    virtual void startScan(JZBLEDeviceDiscoveryAgent *agent) = 0;
    virtual void stopScan(JZBLEDeviceDiscoveryAgent *agent) = 0;
    virtual bool isScanning(JZBLEDeviceDiscoveryAgent *agent) = 0;

    virtual void connectToDevice(JZBLEController *d, const std::vector<JZBleBondKey>& keys) = 0;
    virtual void disconnectFromDevice(JZBLEController *d) = 0;
    virtual bool isConnected(JZBLEController *d) = 0;

    virtual void discoverServices(const std::string& address, JZBLEController *d) = 0;
    virtual std::vector<JZBluetoothUuid> services(const std::string& address) = 0;

    virtual void discoverCharacteristics(const std::string& address, const std::string& serviceUuid,
                                          JZBLEService *d) = 0;
    virtual std::vector<JZLowEnergyCharacteristic> characteristics(
        const std::string& address, const std::string& serviceUuid) = 0;

    virtual void readCharacteristic(const std::string& address, const std::string& serviceUuid,
                                     const std::string& charUuid, JZBLEService *d) = 0;
    virtual void writeCharacteristic(const std::string& address, const std::string& serviceUuid,
                                      const std::string& charUuid, const JZByteArray& value,
                                      bool writeNoResponse, JZBLEService *d) = 0;
    virtual void enableNotification(const std::string& address, const std::string& serviceUuid,
                                     const std::string& charUuid, bool enable, JZBLEService *d) = 0;
    virtual void readDescriptor(const std::string& address, const std::string& serviceUuid,
                                 const std::string& charUuid, const std::string& descUuid,
                                 JZBLEService *d) = 0;
    virtual void writeDescriptor(const std::string& address, const std::string& serviceUuid,
                                  const std::string& charUuid, const std::string& descUuid,
                                  const JZByteArray& value, JZBLEService *d) = 0;

    // === Peripheral ===
    virtual void addPeripheralCharacteristic(const std::string& svcUuid, const std::string& charUuid, int properties) {}
    virtual void startAdvertising(const std::string& name, const std::vector<JZBluetoothUuid>& serviceUuids,
                                   JZBLEPeripheral *d) = 0;
    virtual void stopAdvertising() = 0;
    virtual void peripheralSendNotify(const JZBluetoothUuid& charUuid, const JZByteArray& value) = 0;
    virtual void peripheralSendIndicate(const JZBluetoothUuid& charUuid, const JZByteArray& value) = 0;

    // 主动发起配对（配对完成后密钥通过 sigBondKeyReported 信号上报）
    virtual void requestPairing(const std::string& addr) { (void)addr; }

    // Peripheral 角色：被连方在连接建立前由上层预灌绑定密钥，
    // 以便已配对 Central 重连并请求加密时，storeReadCb 能据此恢复加密会话。
    // Central 角色无需调用本接口（密钥随 connectToDevice(keys) 注入）。
    virtual void setBondKey(const std::string& addr, const JZBleBondKey& key) { (void)addr; (void)key; }

    // === 事件转发中转 (JZBLEController / JZBLEPeripheral 的 friend) ===
    // 具体驱动不得直接调用 JZBLEController / JZBLEPeripheral 的受保护静态分发方法,
    // 统一经由此处转发, 避免在子类中暴露 friend 关系。
    static void centralOnDeviceConnected(JZBLEController *self, const char *addr);
    static void centralOnDeviceDisconnected(JZBLEController *self, const char *addr);
    static void centralOnServicesDiscovered(JZBLEController *self, const char *addr);
    static void centralOnError(JZBLEController *self, int code, const char *msg);
    // 配对完成上报：驱动在 NimBLE 线程捕获密钥后，经此中转切到 Central 线程 emit 信号
    static void centralOnBondKeyReported(JZBLEController *self, const std::string &addr, const JZBleBondKey &key);

    static void peripheralOnClientConnected(JZBLEPeripheral *self);
    static void peripheralOnClientDisconnected(JZBLEPeripheral *self);
    static void peripheralOnCharWritten(JZBLEPeripheral *self, const JZBluetoothUuid &charUuid, const JZByteArray &value);

protected:
    JZ_DISABLE_COPY(JZBleDriver)

    // 当前正在扫描的 Discovery Agent（参考 JZWifiDriverEsp32 的 m_scanTarget）
    JZObjectPointer m_scanAgent;
    // 绑定密钥上报目标由各具体驱动自行维护（按对端地址路由，支持多 Central 并发）。
    static JZBleDriver *s_instance;
};

#endif // !JZ_BLE_DRIVER_H_
