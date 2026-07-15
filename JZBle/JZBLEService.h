#ifndef JZBLESERVICE_H
#define JZBLESERVICE_H

#include "JZObject.h"
#include "JZSignal.h"
#include "JZBluetoothUuid.h"
#include "JZLowEnergyCharacteristic.h"
#include "JZLowEnergyDescriptor.h"

class JZBleDriver;

class JZBLEService : public JZObject
{
    JZ_OBJECT_DECLARE(JZBLEService)

public:
    enum State { Invalid, DiscoveryRequired, Discovering, Discovered };
    enum ErrorCode { NoError, OperationError, ReadError, WriteError, DescriptorReadError, DescriptorWriteError, UnknownError };

    explicit JZBLEService(const JZBluetoothUuid &serviceUuid, JZObject *parent = nullptr);
    ~JZBLEService() override;

    void discoverDetails();
    void readCharacteristic(const JZBluetoothUuid &charUuid);
    void writeCharacteristic(const JZBluetoothUuid &charUuid, const std::string &value, bool writeNoResponse = false);
    void readDescriptor(const JZBluetoothUuid &charUuid, const JZBluetoothUuid &descUuid);
    void writeDescriptor(const JZBluetoothUuid &charUuid, const JZBluetoothUuid &descUuid, const std::string &value);

    std::vector<JZLowEnergyCharacteristic> characteristics() const;
    JZLowEnergyCharacteristic characteristic(const JZBluetoothUuid &uuid) const;
    JZBluetoothUuid serviceUuid() const { return m_serviceUuid; }
    int state() const;
    int error() const;
    std::string errorString() const;

    void setDriver(JZBleDriver *driver);
    void setDeviceAddress(const std::string &address);

    // 驱动回调需直接读取
    JZBluetoothUuid m_serviceUuid;
    std::string     deviceAddress;

    // 静态分发方法（供驱动回调路由，参数为本服务对象指针）
    static void onCharacteristicsDiscovered(JZBLEService *d, const char *addr, const char *svcUuid);
    static void onCharacteristicRead(JZBLEService *d, const char *addr, const char *svcUuid, const char *value);
    static void onCharacteristicWritten(JZBLEService *d, const char *addr, const char *svcUuid, const char *value, bool ok);
    static void onCharacteristicChanged(JZBLEService *d, const char *addr, const char *svcUuid, const char *value);
    static void onDescriptorRead(JZBLEService *d, const char *addr, const char *svcUuid, const char *charUuid, const char *value);
    static void onDescriptorWritten(JZBLEService *d, const char *addr, const char *svcUuid, const char *charUuid, const char *value, bool ok);

jz_signals:
    JZSignal<int> sigStateChanged;
    JZSignal<const JZLowEnergyCharacteristic&, const std::string&> sigCharacteristicRead;
    JZSignal<const JZLowEnergyCharacteristic&, const std::string&> sigCharacteristicWritten;
    JZSignal<const JZLowEnergyCharacteristic&, const std::string&> sigCharacteristicChanged;
    JZSignal<const JZLowEnergyDescriptor&, const std::string&> sigDescriptorRead;
    JZSignal<const JZLowEnergyDescriptor&, const std::string&> sigDescriptorWritten;
    JZSignal<int> sigError;

protected:
    JZ_DISABLE_COPY(JZBLEService)

    JZBleDriver *driver = nullptr;

    State     m_state = DiscoveryRequired;
    ErrorCode m_error = NoError;
    std::string errorMsg;
    std::vector<JZLowEnergyCharacteristic> charList;

    JZLowEnergyCharacteristic findChar(const JZBluetoothUuid &uuid) const;
};

#endif // JZBLESERVICE_H
