#ifndef JZ_BLUETOOTH_UUID_H_
#define JZ_BLUETOOTH_UUID_H_
#include "JZGlobal.h"
#include <string>
#include <stdint.h>

class JZFRAMEWORK_API JZBluetoothUuid
{
public:
    JZBluetoothUuid();
    explicit JZBluetoothUuid(uint32_t uuid32);
    explicit JZBluetoothUuid(const std::string &uuid128);
    JZBluetoothUuid(const JZBluetoothUuid &other);
    ~JZBluetoothUuid();

    JZBluetoothUuid &operator=(const JZBluetoothUuid &other);
    bool operator==(const JZBluetoothUuid &other) const;
    bool operator!=(const JZBluetoothUuid &other) const;
    bool operator<(const JZBluetoothUuid &other) const;

    std::string toString() const;
    uint16_t    toUInt16() const;   // 仅 16-bit UUID 有效
    uint32_t    toUInt32() const;   // 仅 32-bit UUID 有效

    bool isNull() const;

private:
    std::string m_uuid; // 128-bit 标准格式 (xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx)
};

#endif // !JZ_BLUETOOTH_UUID_H_
