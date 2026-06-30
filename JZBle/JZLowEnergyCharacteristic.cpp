#include "JZLowEnergyCharacteristic.h"

JZLowEnergyCharacteristic::JZLowEnergyCharacteristic()
{
}

JZLowEnergyCharacteristic::JZLowEnergyCharacteristic(const JZLowEnergyCharacteristic &other)
    : m_uuid(other.m_uuid),
      m_properties(other.m_properties),
      m_value(other.m_value),
      m_descriptors(other.m_descriptors)
{
}

JZLowEnergyCharacteristic::~JZLowEnergyCharacteristic()
{
}

JZLowEnergyCharacteristic &JZLowEnergyCharacteristic::operator=(const JZLowEnergyCharacteristic &other)
{
    if (this != &other)
    {
        m_uuid        = other.m_uuid;
        m_properties  = other.m_properties;
        m_value       = other.m_value;
        m_descriptors = other.m_descriptors;
    }
    return *this;
}

JZBluetoothUuid JZLowEnergyCharacteristic::uuid() const
{
    return m_uuid;
}

int JZLowEnergyCharacteristic::properties() const
{
    return m_properties;
}

std::string JZLowEnergyCharacteristic::value() const
{
    return m_value;
}

std::vector<JZLowEnergyDescriptor> JZLowEnergyCharacteristic::descriptors() const
{
    return m_descriptors;
}

JZLowEnergyDescriptor JZLowEnergyCharacteristic::descriptor(const JZBluetoothUuid &uuid) const
{
    for (const auto &desc : m_descriptors)
    {
        if (desc.uuid() == uuid)
            return desc;
    }
    return JZLowEnergyDescriptor();
}

bool JZLowEnergyCharacteristic::isValid() const
{
    return !m_uuid.isNull();
}

void JZLowEnergyCharacteristic::setUuid(const JZBluetoothUuid &uuid)
{
    m_uuid = uuid;
}

void JZLowEnergyCharacteristic::setProperties(int props)
{
    m_properties = props;
}

void JZLowEnergyCharacteristic::setValue(const std::string &value)
{
    m_value = value;
}

void JZLowEnergyCharacteristic::setDescriptors(const std::vector<JZLowEnergyDescriptor> &descs)
{
    m_descriptors = descs;
}
