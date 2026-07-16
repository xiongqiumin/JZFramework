#include "JZBluetoothUuid.h"
#include "JZStd.h"
#include <cinttypes>

JZBluetoothUuid::JZBluetoothUuid()
{
}

JZBluetoothUuid::JZBluetoothUuid(uint32_t uuid32)
{
    char buf[64];
    snprintf(buf, sizeof(buf),
             "%08" PRIx32 "-0000-1000-8000-00805f9b34fb",
             uuid32);
    m_uuid = buf;
}

JZBluetoothUuid::JZBluetoothUuid(const std::string &uuid128)
    : m_uuid(uuid128)
{
}

JZBluetoothUuid::JZBluetoothUuid(const JZBluetoothUuid &other)
    : m_uuid(other.m_uuid)
{
}

JZBluetoothUuid::~JZBluetoothUuid()
{
}

JZBluetoothUuid &JZBluetoothUuid::operator=(const JZBluetoothUuid &other)
{
    if (this != &other)
        m_uuid = other.m_uuid;
    return *this;
}

bool JZBluetoothUuid::operator==(const JZBluetoothUuid &other) const
{
    return m_uuid == other.m_uuid;
}

bool JZBluetoothUuid::operator!=(const JZBluetoothUuid &other) const
{
    return m_uuid != other.m_uuid;
}

bool JZBluetoothUuid::operator<(const JZBluetoothUuid &other) const
{
    return m_uuid < other.m_uuid;
}

std::string JZBluetoothUuid::toString() const
{
    return m_uuid;
}

uint16_t JZBluetoothUuid::toUInt16() const
{
    // 解析 0000xxxx-0000-1000-8000-00805F9B34FB 格式
    const std::string prefix = "0000";
    const std::string suffix = "-0000-1000-8000-00805f9b34fb";
    if (str_startsWith(m_uuid, prefix) && str_endsWith(m_uuid, suffix))
    {
        std::string hex = m_uuid.substr(4, 4);
        return (uint16_t)strtol(hex.c_str(), nullptr, 16);
    }
    return 0;
}

uint32_t JZBluetoothUuid::toUInt32() const
{
    const std::string suffix = "-0000-1000-8000-00805f9b34fb";
    if (str_endsWith(m_uuid, suffix) && m_uuid.size() >= 8)
    {
        std::string hex = m_uuid.substr(0, 8);
        return (uint32_t)strtoul(hex.c_str(), nullptr, 16);
    }
    return 0;
}

bool JZBluetoothUuid::isNull() const
{
    return m_uuid.empty();
}
