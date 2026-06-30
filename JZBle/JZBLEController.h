#ifndef JZBLECONTROLLER_H
#define JZBLECONTROLLER_H

#include "JZObject.h"
#include "JZSignal.h"
#include "JZBluetoothDeviceInfo.h"
#include "JZBluetoothUuid.h"

class JZBleAbstractDriver;
class JZBLEService;

class JZBLEControllerPrivate
{
public:
    class JZBLEController *q_ptr;
    JZBLEControllerPrivate(class JZBLEController *q);

    enum State { Unconnected, Connecting, Connected, Discovering, Disconnecting, Error };
    enum ErrorCode { NoError, ConnectionError, ServiceError, UnknownError };

    State     state = Unconnected;
    ErrorCode error = NoError;
    std::string errorMsg;
    JZBluetoothDeviceInfo deviceInfo;
    std::vector<JZBluetoothUuid> serviceList;

    JZBleAbstractDriver *getDriver();

    // 静态分发方法（Driver 直接调用）
    static void onDeviceDiscovered(JZBLEControllerPrivate *d, const JZBluetoothDeviceInfo &info);
    static void onScanFinished(JZBLEControllerPrivate *d);
    static void onDeviceConnected(JZBLEControllerPrivate *d, const char *addr);
    static void onDeviceDisconnected(JZBLEControllerPrivate *d, const char *addr);
    static void onServicesDiscovered(JZBLEControllerPrivate *d, const char *addr);
    static void onError(JZBLEControllerPrivate *d, int code, const char *msg);
    // Peripheral 回调
    static void onClientConnected(JZBLEControllerPrivate *d);
    static void onClientDisconnected(JZBLEControllerPrivate *d);
    static void onCharWritten(JZBLEControllerPrivate *d, const char *charUuid, const char *value);
};

class JZBLEController : public JZObject
{
    JZ_OBJECT_DECLARE(JZBLEController)

public:
    enum ControllerRole { CentralRole, PeripheralRole };

    static JZBLEController *createCentral(const JZBluetoothDeviceInfo &deviceInfo, JZObject *parent = nullptr);
    static JZBLEController *createPeripheral(JZObject *parent = nullptr);

    ~JZBLEController() override;

    ControllerRole role() const;
    int state() const;
    int error() const;
    std::string errorString() const;

    // 主机接口
    void connectToDevice();
    void disconnectFromDevice();
    void discoverServices();
    std::vector<JZBluetoothUuid> services() const;
    JZBLEService *createServiceObject(const JZBluetoothUuid &serviceUuid, JZObject *parent = nullptr);

    // 从机接口
    void addService(const JZBluetoothUuid &serviceUuid);
    void addCharacteristic(const JZBluetoothUuid &serviceUuid, const JZBluetoothUuid &charUuid,
                           int properties, const std::string &initialValue = "");
    void startAdvertising(const std::string &localName = "");
    void stopAdvertising();
    bool isAdvertising() const;

    JZBluetoothDeviceInfo deviceInfo() const;
    void setDeviceInfo(const JZBluetoothDeviceInfo &info);

jz_signals:
    JZSignal<const JZBluetoothDeviceInfo&> sigDeviceDiscovered;
    JZSignal<> sigScanFinished;
    JZSignal<> sigConnected;
    JZSignal<> sigDisconnected;
    JZSignal<> sigDiscoveryFinished;
    JZSignal<int> sigStateChanged;
    JZSignal<int> sigError;
    // Peripheral
    JZSignal<> sigClientConnected;
    JZSignal<> sigClientDisconnected;
    JZSignal<const std::string&, const std::string&> sigCharacteristicWritten;

protected:
    JZ_DISABLE_COPY(JZBLEController)
    JZBLEControllerPrivate *d;
};

#endif // JZBLECONTROLLER_H
