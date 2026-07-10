#ifndef JZBLECENTRAL_H
#define JZBLECENTRAL_H

#include "JZObject.h"
#include "JZSignal.h"
#include "JZBluetoothDeviceInfo.h"
#include "JZBluetoothUuid.h"
#include <string>
#include <vector>

class JZBleDriver;
class JZBLEService;

class JZBLECentral : public JZObject 
{
    JZ_OBJECT_DECLARE(JZBLECentral)

    // 仅允许 BLE 驱动基类访问受保护的静态分发方法,
    // 具体驱动(如 JZBleDriverSimulator/Esp32/Ac79)统一通过 JZBleDriver 的转发中转调用。
    friend class JZBleDriver;
    
public:
    enum State { Unconnected, Connecting, Connected, Discovering };

    JZBLECentral(JZObject *parent = nullptr);
    ~JZBLECentral() override;

    const JZBluetoothDeviceInfo &deviceInfo() const;

    // 主机专属接口
    void connectToDevice(const JZBluetoothDeviceInfo &info);
    void disconnectFromDevice();

    void discoverServices();
    
    std::vector<JZBluetoothUuid> services() const;
    JZBLEService* createService(const JZBluetoothUuid &uuid);

jz_signals:
    JZSignal<> sigConnected;
    JZSignal<> sigDisconnected;
    JZSignal<> sigDiscoveryFinished;
    JZSigString sigError;
    JZSignal<State> sigStateChanged;

protected:
    JZBleDriver *getDriver();

    static void onDeviceConnected(JZBLECentral *self, const char *addr);
    static void onDeviceDisconnected(JZBLECentral *self, const char *addr);
    static void onServicesDiscovered(JZBLECentral *self, const char *addr);
    static void onError(JZBLECentral *self, int code, const char *msg);

    State                     m_state = Unconnected;
    std::string               m_errorMsg;
    JZBluetoothDeviceInfo     m_deviceInfo;
    std::vector<JZBluetoothUuid> m_serviceList;
};

#endif // JZBLECENTRAL_H
