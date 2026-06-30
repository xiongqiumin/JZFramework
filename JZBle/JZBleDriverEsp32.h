#ifndef JZ_BLE_DRIVER_ESP32_H_
#define JZ_BLE_DRIVER_ESP32_H_

#include "JZBleDriver.h"
#include <map>

class JZBleDriverEsp32 : public JZBleAbstractDriver
{
public:
    JZBleDriverEsp32();
    ~JZBleDriverEsp32() override;

    bool isBluetoothAvailable() override;
    bool isBluetoothEnabled() override;

    void startScan() override;
    void stopScan() override;
    bool isScanning() override;

    void connectToDevice(const std::string& address, JZBLEControllerPrivate *d) override;
    void disconnectFromDevice(const std::string& address, JZBLEControllerPrivate *d) override;
    bool isConnected(const std::string& address) override;

    void discoverServices(const std::string& address, JZBLEControllerPrivate *d) override;
    std::vector<JZBluetoothUuid> services(const std::string& address) override;

    void discoverCharacteristics(const std::string& address, const std::string& serviceUuid,
                                  JZBLEServicePrivate *d) override;
    std::vector<JZLowEnergyCharacteristic> characteristics(const std::string& address,
                                                            const std::string& serviceUuid) override;

    void readCharacteristic(const std::string& address, const std::string& serviceUuid,
                             const std::string& charUuid, JZBLEServicePrivate *d) override;
    void writeCharacteristic(const std::string& address, const std::string& serviceUuid,
                              const std::string& charUuid, const std::string& value,
                              bool writeNoResponse, JZBLEServicePrivate *d) override;
    void enableNotification(const std::string& address, const std::string& serviceUuid,
                             const std::string& charUuid, bool enable, JZBLEServicePrivate *d) override;
    void readDescriptor(const std::string& address, const std::string& serviceUuid,
                         const std::string& charUuid, const std::string& descUuid,
                         JZBLEServicePrivate *d) override;
    void writeDescriptor(const std::string& address, const std::string& serviceUuid,
                          const std::string& charUuid, const std::string& descUuid,
                          const std::string& value, JZBLEServicePrivate *d) override;

    void startAdvertising(const std::string& name, const std::vector<JZBluetoothUuid>& serviceUuids,
                           JZBLEControllerPrivate *d) override;
    void stopAdvertising() override;
    void peripheralSendNotify(const std::string& charUuid, const std::string& value) override;
    void peripheralSendIndicate(const std::string& charUuid, const std::string& value) override;

    void addPeripheralCharacteristic(const std::string& svcUuid, const std::string& charUuid, int properties) override;

private:
    static std::string addrToString(const uint8_t *addr);
    static void       addrFromString(const std::string &str, uint8_t *addrOut);
    static int        uuidFromString(const std::string &str, void *uuidOut);
    int  getConnHandle(const std::string &address);
    void setConnHandle(const std::string &address, int handle);
    void removeConnHandle(const std::string &address);

    static int onGapEvent(struct ble_gap_event *event, void *arg);

    bool m_scanning = false;
    bool m_advertising = false;
    JZBLEControllerPrivate *m_disconnectCtrl = nullptr;

    int m_periphConnHandle = -1;  // Peripheral client 连接句柄
    struct PeriphCharInfo { std::string svcUuid; uint16_t valHandle; int properties; };
    std::map<std::string, PeriphCharInfo> m_periphCharMap;  // charUuid → handles/props
    JZBLEControllerPrivate *m_periphCtrlPriv = nullptr;    // 当前 Peripheral Controller
    static int onGattAccess(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt, void *arg);

    std::map<std::string, int> m_connMap;
    std::map<std::string, std::vector<JZBluetoothUuid>> m_svcCache;
    std::map<std::string, std::vector<JZLowEnergyCharacteristic>> m_charCache;
};

#endif // !JZ_BLE_DRIVER_ESP32_H_
