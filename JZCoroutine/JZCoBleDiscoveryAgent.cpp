#include "JZCoBleDiscoveryAgent.h"

JZCoBleDiscoveryAgent::JZCoBleDiscoveryAgent(JZObject *parent)
    : JZObject(parent)
    , m_waitType(WaitNone)
    , m_error(false)
{
    m_agent = new JZBLEDeviceDiscoveryAgent(this);
    connectSignals();
}

JZCoBleDiscoveryAgent::~JZCoBleDiscoveryAgent()
{
    m_agent->stop();
}

void JZCoBleDiscoveryAgent::connectSignals()
{
    connect(&m_agent->sigDeviceDiscovered, this, &JZCoBleDiscoveryAgent::onDeviceDiscovered);
    connect(&m_agent->sigFinished, this, &JZCoBleDiscoveryAgent::onFinished);
    connect(&m_agent->sigError, this, &JZCoBleDiscoveryAgent::onError);
}

void JZCoBleDiscoveryAgent::yield(WaitType type, int timeout)
{
    m_waitType = type;
    m_coCtrl.wait(timeout);
    m_waitType = WaitNone;
}

JZResult<std::vector<JZBLEDeviceInfo>> JZCoBleDiscoveryAgent::scan(int timeout)
{
    m_error = false;
    m_errorMsg.clear();
    m_devices.clear();

    m_agent->setLowEnergyDiscoveryTimeout(timeout);
    m_agent->start();

    yield(WaitScan, timeout);

    m_agent->stop();

    if (m_error)
        return JZResult<std::vector<JZBLEDeviceInfo>>::Error(m_errorMsg);

    return JZResult<std::vector<JZBLEDeviceInfo>>::Ok(m_devices);
}

void JZCoBleDiscoveryAgent::onDeviceDiscovered(const JZBLEDeviceInfo &info)
{
    m_devices.push_back(info);
}

void JZCoBleDiscoveryAgent::onFinished()
{
    if (m_waitType == WaitScan)
        m_coCtrl.wakeAll();
}

void JZCoBleDiscoveryAgent::onError(int code, const std::string &msg)
{
    m_error = true;
    m_errorMsg = msg;
    if (m_waitType == WaitScan)
        m_coCtrl.wakeAll();
}
