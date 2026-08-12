#ifndef JZBLEPERIPHERAL_H
#define JZBLEPERIPHERAL_H

#include "JZObject.h"
#include "JZSignal.h"
#include "JZBluetoothUuid.h"
#include "JZByteArray.h"
#include <string>
#include <vector>

class JZBleDriver;
class JZBleDriverEsp32;

class JZBleServiceData
{
public:
    struct CharData 
    {
        const JZBluetoothUuid &charUuid;
        int properties = 0;
        JZByteArray value;
    };

    void setUuid(const JZBluetoothUuid &uuid); // 设置服务UUID
    const JZBluetoothUuid &uuid() const;

    void addCharacteristic(const JZBluetoothUuid &charUuid, int properties, const JZByteArray &initialValue = JZByteArray());
    const std::vector<CharData> &characteristic() const;

protected:
    JZBluetoothUuid m_uuid;
    std::vector<CharData> m_charList;
};

class JZBLEPeripheralConnect
{
public:
    virtual ~JZBLEPeripheralConnect();
};

class JZBLEPeripheral : public JZObject
{
    JZ_OBJECT_DECLARE(JZBLEPeripheral)

public:
    enum ErrorCode { NoError, ConnectionError, ServiceError, UnknownError };

    JZBLEPeripheral(JZObject *parent = nullptr);
    ~JZBLEPeripheral() override;

    std::string localName();
    void setLocalName(std::string m_localName);

    // 从机专属接口
    void addService(const JZBleServiceData &serviceUuid);
    void startAdvertising();
    void stopAdvertising();
    bool isAdvertising() const;

    void disconnectFromDevice(JZBLEPeripheralConnect *connect);

    void sendNotify(JZBLEPeripheralConnect *connect, const JZBluetoothUuid &charUuid, const JZByteArray &value);
    void sendNotifyToAll(const JZBluetoothUuid &charUuid, const JZByteArray &value);

jz_signals:
    JZSignal<JZBLEPeripheralConnect*> sigConnected;
    JZSignal<JZBLEPeripheralConnect*> sigDisconnected;
    JZSignal<JZBLEPeripheralConnect*, const JZBluetoothUuid&, const JZByteArray&> sigCharacteristicWritten;

protected:
    friend class JZBleDriver;
    friend class JZBleDriverEsp32;

    JZBleDriver *getDriver();

    static void onClientConnected(JZBLEPeripheral *self, std::shared_ptr<JZBLEPeripheralConnect> connect);
    static void onClientDisconnected(JZBLEPeripheral *self, std::shared_ptr<JZBLEPeripheralConnect> connect);
    static void onCharWritten(JZBLEPeripheral *self, std::shared_ptr<JZBLEPeripheralConnect> connect, const JZBluetoothUuid &charUuid, const JZByteArray &value);

    ErrorCode m_error = NoError;
    std::string m_errorMsg;
    std::string m_localName;
    std::vector<JZBleServiceData> m_serviceList;
    std::vector<std::shared_ptr<JZBLEPeripheralConnect>> m_connects;
    bool      m_advertising = false;
};

#endif // JZBLEPERIPHERAL_H
