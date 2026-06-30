#include "JZBluetoothDeviceInfo.h"

JZBluetoothDeviceInfo::JZBluetoothDeviceInfo()
{
}

JZBluetoothDeviceInfo::JZBluetoothDeviceInfo(const JZBluetoothDeviceInfo &other)
    : m_address(other.m_address),
      m_name(other.m_name),
      m_rssi(other.m_rssi),
      m_serviceUuids(other.m_serviceUuids)
{
}

JZBluetoothDeviceInfo::~JZBluetoothDeviceInfo()
{
}

JZBluetoothDeviceInfo &JZBluetoothDeviceInfo::operator=(const JZBluetoothDeviceInfo &other)
{
    if (this != &other)
    {
        m_address       = other.m_address;
        m_name          = other.m_name;
        m_rssi          = other.m_rssi;
        m_serviceUuids  = other.m_serviceUuids;
    }
    return *this;
}

bool JZBluetoothDeviceInfo::operator==(const JZBluetoothDeviceInfo &other) const
{
    return m_address == other.m_address
        && m_name == other.m_name
        && m_rssi == other.m_rssi
        && m_serviceUuids == other.m_serviceUuids;
}

bool JZBluetoothDeviceInfo::operator!=(const JZBluetoothDeviceInfo &other) const
{
    return !(*this == other);
}

std::string JZBluetoothDeviceInfo::address() const
{
    return m_address;
}

void JZBluetoothDeviceInfo::setAddress(const std::string &address)
{
    m_address = address;
}

std::string JZBluetoothDeviceInfo::name() const
{
    return m_name;
}

void JZBluetoothDeviceInfo::setName(const std::string &name)
{
    m_name = name;
}

int JZBluetoothDeviceInfo::rssi() const
{
    return m_rssi;
}

void JZBluetoothDeviceInfo::setRssi(int rssi)
{
    m_rssi = rssi;
}

std::vector<JZBluetoothUuid> JZBluetoothDeviceInfo::serviceUuids() const
{
    return m_serviceUuids;
}

void JZBluetoothDeviceInfo::setServiceUuids(const std::vector<JZBluetoothUuid> &uuids)
{
    m_serviceUuids = uuids;
}

bool JZBluetoothDeviceInfo::isValid() const
{
    return !m_address.empty();
}
