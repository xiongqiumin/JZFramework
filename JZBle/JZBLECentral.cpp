#include "JZBLECentral.h"
#include "JZBLEService.h"
#include "JZBleDriver.h"
#include "JZDebug.h"
#include "JZThread.h"

// 绑定密钥结构需跨线程经信号传递，注册 metatype（此处 JZBleDriver 已完整定义）
JZ_DECLARE_METATYPE(JZBleDriver::JZBleBondKey)

// ==================== JZBLECentral 静态分发 ====================
void JZBLECentral::onDeviceConnected(JZBLECentral *self, const char *addr)
{
    std::string a(addr);
    runOnObject(self, [self, a]() {
        if (a == self->m_deviceInfo.address()) {
            self->m_state = Connected;
            self->sigConnected.emit();
        }
    });
}

void JZBLECentral::onDeviceDisconnected(JZBLECentral *self, const char *addr)
{
    std::string a(addr);
    runOnObject(self, [self, a]() {
        if (a == self->m_deviceInfo.address()) {
            self->m_state = Unconnected;
            self->sigDisconnected.emit();
        }
    });
}

void JZBLECentral::onServicesDiscovered(JZBLECentral *self, const char *addr)
{
    std::string a(addr);
    runOnObject(self, [self, a]() {
        JZBleDriver *drv = JZBleDriver::instance();
        if (a == self->m_deviceInfo.address() && drv) {
            self->m_serviceList = drv->services(a);
            self->m_state = Connected;
            self->sigDiscoveryFinished.emit();
        }
    });
}

void JZBLECentral::onError(JZBLECentral *self, int, const char *msg)
{
    std::string m(msg ? msg : "");
    runOnObject(self, [self, m]() {
        self->m_errorMsg = m;
        self->sigError.emit(m);
    });
}

void JZBLECentral::onBondKeyReported(JZBLECentral *self, const std::string &addr, const JZBleDriver::JZBleBondKey &key)
{
    // 在 NimBLE 主机线程被调用，切到 Central 所在线程后再 emit 信号，保证跨线程安全
    runOnObject(self, [self, addr, key]() {
        self->sigBondKeyReported.emit(addr, key);
    });
}

// ==================== JZBLECentral ====================
JZ_OBJECT_IMPL(JZBLECentral, JZObject)

JZBLECentral::JZBLECentral(JZObject *parent)
    :JZObject(parent),sigConnected(this), sigDisconnected(this), sigDiscoveryFinished(this), sigError(this), sigStateChanged(this), sigBondKeyReported(this)
{
}

JZBLECentral::~JZBLECentral()
{
    disconnectFromDevice();
}

JZBleDriver *JZBLECentral::getDriver()
{
    return JZBleDriver::instance();
}

const JZBluetoothDeviceInfo &JZBLECentral::deviceInfo() const
{
    return m_deviceInfo;
}

// === Central ===
void JZBLECentral::connectToDevice(const JZBluetoothDeviceInfo &deviceInfo)
{
    if (!deviceInfo.isValid()){
        jzWarning() << "JZBluetoothDeviceInfo is invaild";
        return;
    }
    m_deviceInfo = deviceInfo;
    
    auto drv = JZBleDriver::instance();
    if (drv) {
        m_state = Connecting;
        sigStateChanged.emit(m_state);
        drv->connectToDevice(this);
    }
}

void JZBLECentral::disconnectFromDevice()
{
    if (auto *drv = getDriver())
    {
        if (m_deviceInfo.isValid())
        {
            m_state = Unconnected;
            sigStateChanged.emit(m_state);
            drv->disconnectFromDevice(this);
        }
    }
}

void JZBLECentral::discoverServices()
{
    if (auto *drv = getDriver())
    {
        if (!m_deviceInfo.isValid()) return;
        m_state = Discovering;
        sigStateChanged.emit(m_state);
        drv->discoverServices(m_deviceInfo.address(), this);
    }
}

std::vector<JZBluetoothUuid> JZBLECentral::services() const 
{ 
    return m_serviceList; 
}

JZBLEService *JZBLECentral::createService(const JZBluetoothUuid &svcUuid)
{
    auto *s = new JZBLEService(svcUuid, this);
    s->setDriver(getDriver());
    s->setDeviceAddress(m_deviceInfo.address());
    return s;
}
