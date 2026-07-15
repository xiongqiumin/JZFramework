#ifndef JZ_BLE_DEVICE_INFO_H_
#define JZ_BLE_DEVICE_INFO_H_

#include <string>
#include <vector>
#include <stdint.h>
#include "JZBluetoothUuid.h"

class JZBLEDeviceInfo
{
public:
    JZBLEDeviceInfo();
    JZBLEDeviceInfo(const JZBLEDeviceInfo &other);
    ~JZBLEDeviceInfo();

    JZBLEDeviceInfo &operator=(const JZBLEDeviceInfo &other);
    bool operator==(const JZBLEDeviceInfo &other) const;
    bool operator!=(const JZBLEDeviceInfo &other) const;

    std::string address() const;
    void        setAddress(const std::string &address);

    std::string name() const;
    void        setName(const std::string &name);

    int rssi() const;
    void setRssi(int rssi);

    std::vector<JZBluetoothUuid> serviceUuids() const;
    void setServiceUuids(const std::vector<JZBluetoothUuid> &uuids);

    bool isValid() const;

private:
    std::string m_address;
    std::string m_name;
    int         m_rssi = 0;
    std::vector<JZBluetoothUuid> m_serviceUuids;
};

#endif // !JZ_BLE_DEVICE_INFO_H_
