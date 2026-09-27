#ifndef JZ_LOW_ENERGY_CHARACTERISTIC_H_
#define JZ_LOW_ENERGY_CHARACTERISTIC_H_
#include <string>
#include <vector>
#include "JZBluetoothUuid.h"
#include "JZLowEnergyDescriptor.h"

// QLowEnergyCharacteristic::PropertyType
enum JZLowEnergyCharacteristicProperty
{
    CHC_None        = 0x00,
    CHC_Read        = 0x01,
    CHC_Write       = 0x02,
    CHC_Notify      = 0x04,
    CHC_Indicate    = 0x08,
    CHC_WriteNoResponse = 0x10,
};

class JZFRAMEWORK_API JZLowEnergyCharacteristic
{
public:
    JZLowEnergyCharacteristic();
    JZLowEnergyCharacteristic(const JZLowEnergyCharacteristic &other);
    ~JZLowEnergyCharacteristic();

    JZLowEnergyCharacteristic &operator=(const JZLowEnergyCharacteristic &other);

    JZBluetoothUuid  uuid() const;
    int              properties() const;
    std::string      value() const;

    std::vector<JZLowEnergyDescriptor> descriptors() const;
    JZLowEnergyDescriptor              descriptor(const JZBluetoothUuid &uuid) const;

    bool isValid() const;

    // 内部使用
    void setUuid(const JZBluetoothUuid &uuid);
    void setProperties(int props);
    void setValue(const std::string &value);
    void setDescriptors(const std::vector<JZLowEnergyDescriptor> &descs);

private:
    JZBluetoothUuid m_uuid;
    int             m_properties = CHC_None;
    std::string     m_value;
    std::vector<JZLowEnergyDescriptor> m_descriptors;
};

#endif // !JZ_LOW_ENERGY_CHARACTERISTIC_H_
