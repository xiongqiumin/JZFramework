#ifndef JZBLEDEVICEDISCOVERYAGENT_H
#define JZBLEDEVICEDISCOVERYAGENT_H

#include "JZObject.h"
#include "JZSignal.h"
#include "JZBLEDeviceInfo.h"

class JZBleDriver;

class JZBLEDeviceDiscoveryAgent : public JZObject
{
    JZ_OBJECT_DECLARE(JZBLEDeviceDiscoveryAgent)

public:
    enum DiscoveryError
    {
        NoError                 = 0,
        InputOutputError        = 1,
        InvalidBluetoothAdapter = 2,
        UnsupportedPlatform     = 3,
        UnsupportedDiscovery    = 4,
        UnknownError            = 99
    };

    explicit JZBLEDeviceDiscoveryAgent(JZObject *parent = nullptr);
    ~JZBLEDeviceDiscoveryAgent() override;

    bool isActive() const;

    void start();
    void stop();

    void setLowEnergyDiscoveryTimeout(int timeoutMs);
    int  lowEnergyDiscoveryTimeout() const;

    // 静态分发方法（Driver 直接调用，内部 runOnObject 切回本对象线程再 emit）
    static void onDeviceDiscovered(JZBLEDeviceDiscoveryAgent *self, const JZBLEDeviceInfo &info);
    static void onScanFinished(JZBLEDeviceDiscoveryAgent *self);
    static void onError(JZBLEDeviceDiscoveryAgent *self, int code, const char *msg);

jz_signals:
    JZSignal<const JZBLEDeviceInfo&> sigDeviceDiscovered;
    JZSignal<> sigFinished;
    JZSignal<int, const std::string&> sigError;

private:
    JZ_DISABLE_COPY(JZBLEDeviceDiscoveryAgent)
    bool m_active = false;
    int  m_timeout = 8000;
};

#endif // JZBLEDEVICEDISCOVERYAGENT_H
