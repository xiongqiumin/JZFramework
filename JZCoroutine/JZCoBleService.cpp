#include "JZCoBleService.h"

// JZBLEServicePrivate::Discovered 的取值，用于判断发现完成
static const int kBleServiceDiscovered = 3;

JZCoBleService::JZCoBleService(JZBLEService *service, JZObject *parent)
    : JZObject(parent)
    , m_service(service)
    , m_waitType(WaitNone)
    , m_error(false)
    , m_discoverDone(false)
    , m_hasResult(false)
{
    connectSignals();
}

JZCoBleService::~JZCoBleService()
{
}

JZBluetoothUuid JZCoBleService::serviceUuid() const
{
    return m_service->serviceUuid();
}

void JZCoBleService::connectSignals()
{
    connect(&m_service->sigStateChanged, this, &JZCoBleService::onStateChanged);
    connect(&m_service->sigCharacteristicRead, this, &JZCoBleService::onCharacteristicRead);
    connect(&m_service->sigCharacteristicWritten, this, &JZCoBleService::onCharacteristicWritten);
    connect(&m_service->sigCharacteristicChanged, this, &JZCoBleService::onCharacteristicChanged);
    connect(&m_service->sigDescriptorRead, this, &JZCoBleService::onDescriptorRead);
    connect(&m_service->sigDescriptorWritten, this, &JZCoBleService::onDescriptorWritten);
    connect(&m_service->sigError, this, &JZCoBleService::onError);
}

void JZCoBleService::yield(WaitType type, int timeout)
{
    m_waitType = type;
    m_coCtrl.wait(timeout);
    m_waitType = WaitNone;
}

//=======================================================================
// 发现特征值
//=======================================================================
JZResult<std::vector<JZLowEnergyCharacteristic>> JZCoBleService::discoverDetails(int ms)
{
    m_error = false;
    m_errorMsg.clear();
    m_discoverDone = false;

    m_service->discoverDetails();
    yield(WaitDiscover, ms);

    if (m_error)
        return JZResult<std::vector<JZLowEnergyCharacteristic>>::Error(m_errorMsg);
    if (m_discoverDone)
        return JZResult<std::vector<JZLowEnergyCharacteristic>>::Ok(m_service->characteristics());

    return JZResult<std::vector<JZLowEnergyCharacteristic>>::Error("discover timeout");
}

//=======================================================================
// 读取特征
//=======================================================================
JZResult<std::string> JZCoBleService::readCharacteristic(const JZBluetoothUuid &charUuid, int ms)
{
    m_error = false;
    m_errorMsg.clear();
    m_hasResult = false;
    m_resultValue.clear();
    m_waitUuid = charUuid;

    m_service->readCharacteristic(charUuid);
    yield(WaitRead, ms);

    if (m_error)
        return JZResult<std::string>::Error(m_errorMsg);
    if (m_hasResult && m_waitUuid == charUuid)
        return JZResult<std::string>::Ok(m_resultValue);

    return JZResult<std::string>::Error("read timeout");
}

//=======================================================================
// 写入特征
//=======================================================================
JZResult<void> JZCoBleService::writeCharacteristic(const JZBluetoothUuid &charUuid,
                                                    const std::string &value,
                                                    bool writeNoResponse,
                                                    int ms)
{
    m_error = false;
    m_errorMsg.clear();

    m_service->writeCharacteristic(charUuid, value, writeNoResponse);
    if (writeNoResponse)
        return JZResult<void>::Ok();

    m_hasResult = false;
    m_resultValue.clear();
    m_waitUuid = charUuid;

    yield(WaitWrite, ms);

    if (m_error)
        return JZResult<void>::Error(m_errorMsg);
    if (m_hasResult && m_waitUuid == charUuid)
        return JZResult<void>::Ok();

    return JZResult<void>::Error("write timeout");
}

//=======================================================================
// 读/写描述符
//=======================================================================
JZResult<std::string> JZCoBleService::readDescriptor(const JZBluetoothUuid &charUuid,
                                                      const JZBluetoothUuid &descUuid,
                                                      int ms)
{
    m_error = false;
    m_errorMsg.clear();
    m_hasResult = false;
    m_resultValue.clear();
    m_waitUuid = descUuid;

    m_service->readDescriptor(charUuid, descUuid);
    yield(WaitDescRead, ms);

    if (m_error)
        return JZResult<std::string>::Error(m_errorMsg);
    if (m_hasResult && m_waitUuid == descUuid)
        return JZResult<std::string>::Ok(m_resultValue);

    return JZResult<std::string>::Error("read descriptor timeout");
}

JZResult<void> JZCoBleService::writeDescriptor(const JZBluetoothUuid &charUuid,
                                                const JZBluetoothUuid &descUuid,
                                                const std::string &value,
                                                int ms)
{
    m_error = false;
    m_errorMsg.clear();

    m_service->writeDescriptor(charUuid, descUuid, value);

    m_hasResult = false;
    m_resultValue.clear();
    m_waitUuid = descUuid;

    yield(WaitDescWrite, ms);

    if (m_error)
        return JZResult<void>::Error(m_errorMsg);
    if (m_hasResult && m_waitUuid == descUuid)
        return JZResult<void>::Ok();

    return JZResult<void>::Error("write descriptor timeout");
}

//=======================================================================
// 等待通知/指示
//=======================================================================
JZResult<std::string> JZCoBleService::waitForChanged(const JZBluetoothUuid &charUuid, int ms)
{
    auto it = m_changedMap.find(charUuid);
    if (it != m_changedMap.end() && !it->second.empty()) {
        std::string v = it->second.front();
        it->second.pop();
        return JZResult<std::string>::Ok(v);
    }

    m_waitUuid = charUuid;
    yield(WaitChanged, ms);

    auto it2 = m_changedMap.find(charUuid);
    if (it2 != m_changedMap.end() && !it2->second.empty()) {
        std::string v = it2->second.front();
        it2->second.pop();
        return JZResult<std::string>::Ok(v);
    }

    if (m_error)
        return JZResult<std::string>::Error(m_errorMsg);

    return JZResult<std::string>::Error("wait for changed timeout");
}

//=======================================================================
// 信号处理
//=======================================================================
void JZCoBleService::onStateChanged(int state)
{
    if (state == kBleServiceDiscovered) {
        m_discoverDone = true;
        if (m_waitType == WaitDiscover)
            m_coCtrl.wakeAll();
    }
}

void JZCoBleService::onCharacteristicRead(const JZLowEnergyCharacteristic &ch, const std::string &value)
{
    m_hasResult = true;
    m_resultValue = value;
    m_waitUuid = ch.uuid();
    if (m_waitType == WaitRead)
        m_coCtrl.wakeAll();
}

void JZCoBleService::onCharacteristicWritten(const JZLowEnergyCharacteristic &ch, const std::string &value)
{
    m_hasResult = true;
    m_resultValue = value;
    m_waitUuid = ch.uuid();
    if (m_waitType == WaitWrite)
        m_coCtrl.wakeAll();
}

void JZCoBleService::onCharacteristicChanged(const JZLowEnergyCharacteristic &ch, const std::string &value)
{
    m_changedMap[ch.uuid()].push(value);
    if (m_waitType == WaitChanged && m_waitUuid == ch.uuid())
        m_coCtrl.wakeAll();
}

void JZCoBleService::onDescriptorRead(const JZLowEnergyDescriptor &desc, const std::string &value)
{
    m_hasResult = true;
    m_resultValue = value;
    m_waitUuid = desc.uuid();
    if (m_waitType == WaitDescRead)
        m_coCtrl.wakeAll();
}

void JZCoBleService::onDescriptorWritten(const JZLowEnergyDescriptor &desc, const std::string &value)
{
    m_hasResult = true;
    m_resultValue = value;
    m_waitUuid = desc.uuid();
    if (m_waitType == WaitDescWrite)
        m_coCtrl.wakeAll();
}

void JZCoBleService::onError(int)
{
    m_error = true;
    m_errorMsg = "ble service error";
    if (m_waitType != WaitNone)
        m_coCtrl.wakeAll();
}
