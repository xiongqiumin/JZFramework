#include "JZBLEDeviceDiscoveryAgent.h"
#include "JZBleDriver.h"
#include "JZThread.h"

// ==================== 静态分发（Driver 线程 → 本对象线程 emit） ====================
void JZBLEDeviceDiscoveryAgent::onDeviceDiscovered(JZBLEDeviceDiscoveryAgent *self, const JZBLEDeviceInfo &info)
{
    JZBLEDeviceInfo c = info;
    runOnObject(self, [self, c]() { self->sigDeviceDiscovered.emit(c); });
}
void JZBLEDeviceDiscoveryAgent::onScanFinished(JZBLEDeviceDiscoveryAgent *self)
{
    runOnObject(self, [self]() { self->sigFinished.emit(); });
}
void JZBLEDeviceDiscoveryAgent::onError(JZBLEDeviceDiscoveryAgent *self, int code, const char *msg)
{
    std::string m(msg ? msg : "");
    runOnObject(self, [self, code, m]() { self->sigError.emit(code, m); });
}

// ==================== JZBLEDeviceDiscoveryAgent ====================
JZ_OBJECT_IMPL(JZBLEDeviceDiscoveryAgent, JZObject)

JZBLEDeviceDiscoveryAgent::JZBLEDeviceDiscoveryAgent(JZObject *parent)
    : JZObject(parent),
      sigDeviceDiscovered(this), sigFinished(this), sigError(this),
      m_active(false), m_timeout(8000)
{
}

JZBLEDeviceDiscoveryAgent::~JZBLEDeviceDiscoveryAgent()
{
    stop();
}

bool JZBLEDeviceDiscoveryAgent::isActive() const { return m_active; }

void JZBLEDeviceDiscoveryAgent::start()
{
    JZBleDriver *drv = JZBleDriver::instance();
    if (m_active) return;
    m_active = true;
    drv->startScan(this);
}

void JZBLEDeviceDiscoveryAgent::stop()
{
    JZBleDriver *drv = JZBleDriver::instance();
    m_active = false;
    drv->stopScan(this);
}

void JZBLEDeviceDiscoveryAgent::setLowEnergyDiscoveryTimeout(int timeoutMs) { m_timeout = timeoutMs; }
int  JZBLEDeviceDiscoveryAgent::lowEnergyDiscoveryTimeout() const { return m_timeout; }
