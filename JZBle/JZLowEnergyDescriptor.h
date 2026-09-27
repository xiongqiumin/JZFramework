#ifndef JZ_LOW_ENERGY_DESCRIPTOR_H_
#define JZ_LOW_ENERGY_DESCRIPTOR_H_
#include <string>
#include "JZBluetoothUuid.h"

class JZFRAMEWORK_API JZLowEnergyDescriptor
{
public:
    JZLowEnergyDescriptor();
    JZLowEnergyDescriptor(const JZBluetoothUuid &uuid, const std::string &value);
    JZLowEnergyDescriptor(const JZLowEnergyDescriptor &other);
    ~JZLowEnergyDescriptor();

    JZLowEnergyDescriptor &operator=(const JZLowEnergyDescriptor &other);

    JZBluetoothUuid  uuid() const;
    std::string      value() const;
    bool             isValid() const;

private:
    JZBluetoothUuid m_uuid;
    std::string     m_value;
};

#endif // !JZ_LOW_ENERGY_DESCRIPTOR_H_
