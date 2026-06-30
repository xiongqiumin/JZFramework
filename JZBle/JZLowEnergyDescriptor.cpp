#include "JZLowEnergyDescriptor.h"

JZLowEnergyDescriptor::JZLowEnergyDescriptor()
{
}

JZLowEnergyDescriptor::JZLowEnergyDescriptor(const JZBluetoothUuid &uuid, const std::string &value)
    : m_uuid(uuid), m_value(value)
{
}

JZLowEnergyDescriptor::JZLowEnergyDescriptor(const JZLowEnergyDescriptor &other)
    : m_uuid(other.m_uuid), m_value(other.m_value)
{
}

JZLowEnergyDescriptor::~JZLowEnergyDescriptor()
{
}

JZLowEnergyDescriptor &JZLowEnergyDescriptor::operator=(const JZLowEnergyDescriptor &other)
{
    if (this != &other)
    {
        m_uuid  = other.m_uuid;
        m_value = other.m_value;
    }
    return *this;
}

JZBluetoothUuid JZLowEnergyDescriptor::uuid() const
{
    return m_uuid;
}

std::string JZLowEnergyDescriptor::value() const
{
    return m_value;
}

bool JZLowEnergyDescriptor::isValid() const
{
    return !m_uuid.isNull();
}
