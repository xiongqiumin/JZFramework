#ifndef JZBLEPERIPHERAL_H
#define JZBLEPERIPHERAL_H

#include "JZObject.h"
#include "JZSignal.h"
#include "JZBluetoothUuid.h"
#include "JZByteArray.h"
#include <string>
#include <vector>

class JZBleDriver;

class JZBLEPeripheral : public JZObject
{
    JZ_OBJECT_DECLARE(JZBLEPeripheral)

    // 仅允许 BLE 驱动基类访问受保护的静态分发方法,
    // 具体驱动统一通过 JZBleDriver 的转发中转调用。
    friend class JZBleDriver;

public:
    enum State { Unconnected, Connecting, Connected, Discovering, Disconnecting, Error };
    enum ErrorCode { NoError, ConnectionError, ServiceError, UnknownError };

    JZBLEPeripheral(JZObject *parent = nullptr);
    ~JZBLEPeripheral() override;

    // 从机专属接口
    void addService(const JZBluetoothUuid &serviceUuid);
    void addCharacteristic(const JZBluetoothUuid &serviceUuid,
                           const JZBluetoothUuid &charUuid,
                           int properties,
                           const std::string &initialValue = "");
    void startAdvertising(const std::string &localName = "");
    void stopAdvertising();
    bool isAdvertising() const;

    // 从机数据推送（value 为二进制载荷，使用 JZByteArray 以支持任意字节）
    void sendNotification(const JZBluetoothUuid &charUuid, const JZByteArray &value);

    int state() const;
    int error() const;
    std::string errorString() const;

jz_signals:
    JZSignal<> sigClientConnected;
    JZSignal<> sigClientDisconnected;
    // (写入数据的特征 UUID, 写入的二进制值)
    JZSignal<const JZBluetoothUuid&, const JZByteArray&> sigCharacteristicWritten;

protected:
    JZBleDriver *getDriver();

    static void onClientConnected(JZBLEPeripheral *self);
    static void onClientDisconnected(JZBLEPeripheral *self);
    static void onCharWritten(JZBLEPeripheral *self, const JZBluetoothUuid &charUuid, const JZByteArray &value);

    State     m_state = Unconnected;
    ErrorCode m_error = NoError;
    std::string m_errorMsg;
    std::vector<JZBluetoothUuid> m_serviceList;
    bool      m_advertising = false;
};

#endif // JZBLEPERIPHERAL_H
