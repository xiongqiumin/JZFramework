#ifndef JZ_BLUETOOTH_DEVICE_INFO_H_
#define JZ_BLUETOOTH_DEVICE_INFO_H_

#include <string>
#include <vector>
#include <stdint.h>
#include "JZBluetoothUuid.h"

class JZBluetoothDeviceInfo
{
public:
    JZBluetoothDeviceInfo();
    JZBluetoothDeviceInfo(const JZBluetoothDeviceInfo &other);
    ~JZBluetoothDeviceInfo();

    JZBluetoothDeviceInfo &operator=(const JZBluetoothDeviceInfo &other);
    bool operator==(const JZBluetoothDeviceInfo &other) const;
    bool operator!=(const JZBluetoothDeviceInfo &other) const;

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

#endif // !JZ_BLUETOOTH_DEVICE_INFO_H_
