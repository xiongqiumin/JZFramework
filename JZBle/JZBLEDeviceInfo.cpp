#include "JZBLEDeviceInfo.h"

JZBLEDeviceInfo::JZBLEDeviceInfo()
{
}

JZBLEDeviceInfo::JZBLEDeviceInfo(const JZBLEDeviceInfo &other)
    : m_address(other.m_address),
      m_name(other.m_name),
      m_rssi(other.m_rssi),
      m_serviceUuids(other.m_serviceUuids)
{
}

JZBLEDeviceInfo::~JZBLEDeviceInfo()
{
}

JZBLEDeviceInfo &JZBLEDeviceInfo::operator=(const JZBLEDeviceInfo &other)
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

bool JZBLEDeviceInfo::operator==(const JZBLEDeviceInfo &other) const
{
    return m_address == other.m_address
        && m_name == other.m_name
        && m_rssi == other.m_rssi
        && m_serviceUuids == other.m_serviceUuids;
}

bool JZBLEDeviceInfo::operator!=(const JZBLEDeviceInfo &other) const
{
    return !(*this == other);
}

std::string JZBLEDeviceInfo::address() const
{
    return m_address;
}

void JZBLEDeviceInfo::setAddress(const std::string &address)
{
    m_address = address;
}

std::string JZBLEDeviceInfo::name() const
{
    return m_name;
}

void JZBLEDeviceInfo::setName(const std::string &name)
{
    m_name = name;
}

int JZBLEDeviceInfo::rssi() const
{
    return m_rssi;
}

void JZBLEDeviceInfo::setRssi(int rssi)
{
    m_rssi = rssi;
}

std::vector<JZBluetoothUuid> JZBLEDeviceInfo::serviceUuids() const
{
    return m_serviceUuids;
}

void JZBLEDeviceInfo::setServiceUuids(const std::vector<JZBluetoothUuid> &uuids)
{
    m_serviceUuids = uuids;
}

bool JZBLEDeviceInfo::isValid() const
{
    return !m_address.empty();
}
