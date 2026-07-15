#include "JZCoBleCentral.h"
#include "JZCoBleService.h"

JZCoBleCentral::JZCoBleCentral(JZObject *parent)
    : JZObject(parent)
    , m_waitType(WaitNone)
    , m_connected(false)
    , m_discoveryFinished(false)
    , m_error(false)
{
    m_central = new JZBLEController(this);
    connectSignals();
}

JZCoBleCentral::~JZCoBleCentral()
{
    disconnectFromDevice();
}

void JZCoBleCentral::connectSignals()
{
    connect(&m_central->sigConnected, this, &JZCoBleCentral::onConnected);
    connect(&m_central->sigDisconnected, this, &JZCoBleCentral::onDisconnected);
    connect(&m_central->sigDiscoveryFinished, this, &JZCoBleCentral::onDiscoveryFinished);
    connect(&m_central->sigError, this, &JZCoBleCentral::onError);
}

void JZCoBleCentral::yield(WaitType type, int timeout)
{
    m_waitType = type;
    m_coCtrl.wait(timeout);
    m_waitType = WaitNone;
}

//=======================================================================
// 连接设备
//=======================================================================
JZResult<void> JZCoBleCentral::connectToDevice(const JZBLEDeviceInfo &info, int timeout)
{
    if (m_connected)
        return JZResult<void>::Error("already connected");

    m_error = false;
    m_errorMsg.clear();
    m_connected = false;

    yield(WaitConnect, timeout);

    if (m_error)
        return JZResult<void>::Error(m_errorMsg);
    if (m_connected)
        return JZResult<void>::Ok();

    return JZResult<void>::Error("connect timeout");
}

void JZCoBleCentral::disconnectFromDevice()
{
    m_central->disconnectFromDevice();
}

//=======================================================================
// 发现服务
//=======================================================================
JZResult<std::vector<JZBluetoothUuid>> JZCoBleCentral::discoverServices(int timeout)
{
    m_error = false;
    m_errorMsg.clear();
    m_discoveryFinished = false;

    m_central->discoverServices();
    yield(WaitDiscover, timeout);

    if (m_error)
        return JZResult<std::vector<JZBluetoothUuid>>::Error(m_errorMsg);
    if (m_discoveryFinished)
        return JZResult<std::vector<JZBluetoothUuid>>::Ok(m_central->services());

    return JZResult<std::vector<JZBluetoothUuid>>::Error("discover timeout");
}

const JZBLEDeviceInfo &JZCoBleCentral::deviceInfo() const
{
    return m_central->deviceInfo();
}

JZCoBleService *JZCoBleCentral::createService(const JZBluetoothUuid &uuid)
{
    JZBLEService *svc = m_central->createService(uuid);
    if (!svc)
        return nullptr;
    return new JZCoBleService(svc, this);
}

//=======================================================================
// 信号处理
//=======================================================================
void JZCoBleCentral::onConnected()
{
    m_connected = true;
    if (m_waitType == WaitConnect)
        m_coCtrl.wakeAll();
}

void JZCoBleCentral::onDisconnected()
{
    m_connected = false;
    if (m_waitType != WaitNone) {
        m_error = true;
        m_errorMsg = "disconnected";
        m_coCtrl.wakeAll();
    }
}

void JZCoBleCentral::onDiscoveryFinished()
{
    m_discoveryFinished = true;
    if (m_waitType == WaitDiscover)
        m_coCtrl.wakeAll();
}

void JZCoBleCentral::onError(const std::string &msg)
{
    m_error = true;
    m_errorMsg = msg;
    if (m_waitType != WaitNone)
        m_coCtrl.wakeAll();
}
