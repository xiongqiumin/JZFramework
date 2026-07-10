#ifndef JZBLEDEVICEDISCOVERYAGENT_H
#define JZBLEDEVICEDISCOVERYAGENT_H

#include "JZObject.h"
#include "JZSignal.h"
#include "JZBluetoothDeviceInfo.h"

class JZBleDriver;

class JZBLEDeviceDiscoveryAgentPrivate
{
public:
    class JZBLEDeviceDiscoveryAgent *q_ptr;
    JZBLEDeviceDiscoveryAgentPrivate(class JZBLEDeviceDiscoveryAgent *q);

    // 静态分发方法（Driver 直接调用）
    static void onDeviceDiscovered(JZBLEDeviceDiscoveryAgentPrivate *d, const JZBluetoothDeviceInfo &info);
    static void onScanFinished(JZBLEDeviceDiscoveryAgentPrivate *d);
    static void onError(JZBLEDeviceDiscoveryAgentPrivate *d, int code, const char *msg);
};

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

jz_signals:
    JZSignal<const JZBluetoothDeviceInfo&> sigDeviceDiscovered;
    JZSignal<> sigFinished;
    JZSignal<int, const std::string&> sigError;

private:
    JZ_DISABLE_COPY(JZBLEDeviceDiscoveryAgent)
    JZBLEDeviceDiscoveryAgentPrivate *d;
    bool m_active = false;
    int  m_timeout = 8000;
};

#endif // JZBLEDEVICEDISCOVERYAGENT_H
