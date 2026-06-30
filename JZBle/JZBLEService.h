#ifndef JZBLESERVICE_H
#define JZBLESERVICE_H

#include "JZObject.h"
#include "JZSignal.h"
#include "JZBluetoothUuid.h"
#include "JZLowEnergyCharacteristic.h"
#include "JZLowEnergyDescriptor.h"

class JZBleAbstractDriver;

class JZBLEServicePrivate
{
public:
    class JZBLEService *q_ptr;
    JZBluetoothUuid     serviceUuid;
    JZBleAbstractDriver *driver = nullptr;
    std::string         deviceAddress;

    enum State { Invalid, DiscoveryRequired, Discovering, Discovered };
    enum ErrorCode { NoError, OperationError, ReadError, WriteError, DescriptorReadError, DescriptorWriteError, UnknownError };

    State     state = DiscoveryRequired;
    ErrorCode error = NoError;
    std::string errorMsg;
    std::vector<JZLowEnergyCharacteristic> charList;

    JZBLEServicePrivate(class JZBLEService *q, const JZBluetoothUuid &uuid);

    JZLowEnergyCharacteristic findChar(const JZBluetoothUuid &uuid) const;

    // 静态分发方法
    static void onCharacteristicsDiscovered(JZBLEServicePrivate *d, const char *addr, const char *svcUuid);
    static void onCharacteristicRead(JZBLEServicePrivate *d, const char *addr, const char *svcUuid, const char *value);
    static void onCharacteristicWritten(JZBLEServicePrivate *d, const char *addr, const char *svcUuid, const char *value, bool ok);
    static void onCharacteristicChanged(JZBLEServicePrivate *d, const char *addr, const char *svcUuid, const char *value);
    static void onDescriptorRead(JZBLEServicePrivate *d, const char *addr, const char *svcUuid, const char *charUuid, const char *value);
    static void onDescriptorWritten(JZBLEServicePrivate *d, const char *addr, const char *svcUuid, const char *charUuid, const char *value, bool ok);
};

class JZBLEService : public JZObject
{
    JZ_OBJECT_DECLARE(JZBLEService)

public:
    explicit JZBLEService(const JZBluetoothUuid &serviceUuid, JZObject *parent = nullptr);
    ~JZBLEService() override;

    void discoverDetails();
    void readCharacteristic(const JZBluetoothUuid &charUuid);
    void writeCharacteristic(const JZBluetoothUuid &charUuid, const std::string &value, bool writeNoResponse = false);
    void readDescriptor(const JZBluetoothUuid &charUuid, const JZBluetoothUuid &descUuid);
    void writeDescriptor(const JZBluetoothUuid &charUuid, const JZBluetoothUuid &descUuid, const std::string &value);

    JZBluetoothUuid serviceUuid() const;
    std::vector<JZLowEnergyCharacteristic> characteristics() const;
    JZLowEnergyCharacteristic characteristic(const JZBluetoothUuid &uuid) const;
    int state() const;
    int error() const;
    std::string errorString() const;

    void setDriver(JZBleAbstractDriver *driver);
    void setDeviceAddress(const std::string &address);

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
    JZBLEServicePrivate *d;
};

#endif // JZBLESERVICE_H
