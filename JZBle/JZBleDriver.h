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
class JZBLEServicePrivate;
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
                                   JZBLEPeripheral *d) = 0;
    virtual void stopAdvertising() = 0;
    virtual void peripheralSendNotify(const std::string& charUuid, const std::string& value) = 0;
    virtual void peripheralSendIndicate(const std::string& charUuid, const std::string& value) = 0;

protected:
    JZ_DISABLE_COPY(JZBleDriver)

    // 扫描结果多播给 Discovery Agent
    std::vector<JZBLEDeviceDiscoveryAgentPrivate*> m_agents;
    static JZBleDriver *s_instance;
};

#endif // !JZ_BLE_DRIVER_H_
