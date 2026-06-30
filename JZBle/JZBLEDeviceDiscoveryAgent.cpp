#include "JZBLEDeviceDiscoveryAgent.h"
#include "JZBleDriver.h"
#include "JZThread.h"

// ==================== JZBLEDeviceDiscoveryAgentPrivate ====================
JZBLEDeviceDiscoveryAgentPrivate::JZBLEDeviceDiscoveryAgentPrivate(class JZBLEDeviceDiscoveryAgent *q) : q_ptr(q) {}

void JZBLEDeviceDiscoveryAgentPrivate::onDeviceDiscovered(JZBLEDeviceDiscoveryAgentPrivate *d, const JZBluetoothDeviceInfo &info)
{
    JZBluetoothDeviceInfo c = info;
    runOnObject(d->q_ptr, [d, c]() { d->q_ptr->sigDeviceDiscovered.emit(c); });
}
void JZBLEDeviceDiscoveryAgentPrivate::onScanFinished(JZBLEDeviceDiscoveryAgentPrivate *d)
{
    runOnObject(d->q_ptr, [d]() { d->q_ptr->sigFinished.emit(); });
}
void JZBLEDeviceDiscoveryAgentPrivate::onError(JZBLEDeviceDiscoveryAgentPrivate *d, int code, const char *msg)
{
    std::string m(msg ? msg : "");
    runOnObject(d->q_ptr, [d, code, m]() { d->q_ptr->sigError.emit(code, m); });
}

// ==================== JZBLEDeviceDiscoveryAgent ====================
JZ_OBJECT_IMPL(JZBLEDeviceDiscoveryAgent, JZObject)

JZBLEDeviceDiscoveryAgent::JZBLEDeviceDiscoveryAgent(JZObject *parent)
    : JZObject(parent),
      sigDeviceDiscovered(this), sigFinished(this), sigError(this),
      d(new JZBLEDeviceDiscoveryAgentPrivate(this)), m_active(false), m_timeout(8000)
{
    if (auto *drv = JZBleAbstractDriver::instance())
        drv->registerAgent(d);
}

JZBLEDeviceDiscoveryAgent::~JZBLEDeviceDiscoveryAgent()
{
    stop();
    if (auto *drv = JZBleAbstractDriver::instance())
        drv->unregisterAgent(d);
    delete d;
}

bool JZBLEDeviceDiscoveryAgent::isActive() const { return m_active; }

void JZBLEDeviceDiscoveryAgent::start()
{
    JZBleAbstractDriver *drv = JZBleAbstractDriver::instance();
    if (!drv) return;
    if (!drv->isBluetoothAvailable())
    {
        sigError.emit(InvalidBluetoothAdapter, "Bluetooth not available");
        return;
    }
    if (!drv->isBluetoothEnabled())
    {
        sigError.emit(InvalidBluetoothAdapter, "Bluetooth not enabled");
        return;
    }
    if (m_active) return;
    m_active = true;
    drv->startScan();
}

void JZBLEDeviceDiscoveryAgent::stop()
{
    JZBleAbstractDriver *drv = JZBleAbstractDriver::instance();
    if (!drv) return;
    m_active = false;
    drv->stopScan();
}

void JZBLEDeviceDiscoveryAgent::setLowEnergyDiscoveryTimeout(int timeoutMs) { m_timeout = timeoutMs; }
int  JZBLEDeviceDiscoveryAgent::lowEnergyDiscoveryTimeout() const { return m_timeout; }
