#ifndef JZ_BLE_DRIVER_H_
#define JZ_BLE_DRIVER_H_

#include <string>
#include <vector>
#include "JZStd.h"
#include "JZBluetoothDeviceInfo.h"
#include "JZBluetoothUuid.h"
#include "JZLowEnergyCharacteristic.h"
#include "JZLowEnergyDescriptor.h"

class JZBLEControllerPrivate;
class JZBLEServicePrivate;
class JZBLEDeviceDiscoveryAgentPrivate;

class JZBleAbstractDriver
{
public:
    JZBleAbstractDriver() = default;
    virtual ~JZBleAbstractDriver() = default;

    static JZBleAbstractDriver *instance();
    static void setInstance(JZBleAbstractDriver *inst);

    virtual bool isBluetoothAvailable() = 0;
    virtual bool isBluetoothEnabled() = 0;

    // === Central: 每个异步调用带 Private 指针，事件回传时透传 ===

    // 扫描（注册接收者，扫描结果发给已注册的 Controller / Agent）
    void registerController(JZBLEControllerPrivate *d);
    void unregisterController(JZBLEControllerPrivate *d);
    void registerAgent(JZBLEDeviceDiscoveryAgentPrivate *d);
    void unregisterAgent(JZBLEDeviceDiscoveryAgentPrivate *d);

    virtual void startScan() = 0;
    virtual void stopScan() = 0;
    virtual bool isScanning() = 0;

    virtual void connectToDevice(const std::string& address, JZBLEControllerPrivate *d) = 0;
    virtual void disconnectFromDevice(const std::string& address, JZBLEControllerPrivate *d) = 0;
    virtual bool isConnected(const std::string& address) = 0;

    virtual void discoverServices(const std::string& address, JZBLEControllerPrivate *d) = 0;
    virtual std::vector<JZBluetoothUuid> services(const std::string& address) = 0;

    virtual void discoverCharacteristics(const std::string& address, const std::string& serviceUuid,
                                          JZBLEServicePrivate *d) = 0;
    virtual std::vector<JZLowEnergyCharacteristic> characteristics(
        const std::string& address, const std::string& serviceUuid) = 0;

    virtual void readCharacteristic(const std::string& address, const std::string& serviceUuid,
                                     const std::string& charUuid, JZBLEServicePrivate *d) = 0;
    virtual void writeCharacteristic(const std::string& address, const std::string& serviceUuid,
                                      const std::string& charUuid, const std::string& value,
                                      bool writeNoResponse, JZBLEServicePrivate *d) = 0;
    virtual void enableNotification(const std::string& address, const std::string& serviceUuid,
                                     const std::string& charUuid, bool enable, JZBLEServicePrivate *d) = 0;
    virtual void readDescriptor(const std::string& address, const std::string& serviceUuid,
                                 const std::string& charUuid, const std::string& descUuid,
                                 JZBLEServicePrivate *d) = 0;
    virtual void writeDescriptor(const std::string& address, const std::string& serviceUuid,
                                  const std::string& charUuid, const std::string& descUuid,
                                  const std::string& value, JZBLEServicePrivate *d) = 0;

    // === Peripheral ===
    virtual void addPeripheralCharacteristic(const std::string& svcUuid, const std::string& charUuid, int properties) {}
    virtual void startAdvertising(const std::string& name, const std::vector<JZBluetoothUuid>& serviceUuids,
                                   JZBLEControllerPrivate *d) = 0;
    virtual void stopAdvertising() = 0;
    virtual void peripheralSendNotify(const std::string& charUuid, const std::string& value) = 0;
    virtual void peripheralSendIndicate(const std::string& charUuid, const std::string& value) = 0;

protected:
    JZ_DISABLE_COPY(JZBleAbstractDriver)

    // 扫描结果多播
    std::vector<JZBLEControllerPrivate*>           m_controllers;
    std::vector<JZBLEDeviceDiscoveryAgentPrivate*> m_agents;
    static JZBleAbstractDriver *s_instance;
};

#endif // !JZ_BLE_DRIVER_H_
