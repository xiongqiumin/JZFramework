#include "JZBLEController.h"
#include "JZBLEService.h"
#include "JZBleDriver.h"
#include "JZDebug.h"
#include "JZThread.h"

// 绑定密钥结构需跨线程经信号传递，注册 metatype（此处 JZBleDriver 已完整定义）
JZ_DECLARE_METATYPE(JZBleDriver::JZBleBondKey)

// ==================== JZBLEController 静态分发 ====================
void JZBLEController::onDeviceConnected(JZBLEController *self, const char *addr)
{
    std::string a(addr);
    runOnObject(self, [self, a]() {
        if (a == self->m_deviceInfo.address()) {
            self->m_state = Connected;
            self->sigConnected.emit();
        }
    });
}

void JZBLEController::onDeviceDisconnected(JZBLEController *self, const char *addr)
{
    std::string a(addr);
    runOnObject(self, [self, a]() {
        if (a == self->m_deviceInfo.address()) {
            self->m_state = Unconnected;
            self->sigDisconnected.emit();
        }
    });
}

void JZBLEController::onServicesDiscovered(JZBLEController *self, const char *addr)
{
    std::string a(addr);
    runOnObject(self, [self, a]() {
        JZBleDriver *drv = JZBleDriver::instance();
        if (a == self->m_deviceInfo.address()) {
            self->m_serviceList = drv->services(a);
            self->m_state = Connected;
            self->sigDiscoveryFinished.emit();
        }
    });
}

void JZBLEController::onError(JZBLEController *self, int, const char *msg)
{
    std::string m(msg ? msg : "");
    runOnObject(self, [self, m]() {
        self->m_errorMsg = m;
        self->sigError.emit(m);
    });
}

void JZBLEController::onBondKeyReported(JZBLEController *self, const std::string &addr, const JZBleDriver::JZBleBondKey &key)
{
    // 在 NimBLE 主机线程被调用，切到 Central 所在线程后再 emit 信号，保证跨线程安全
    runOnObject(self, [self, addr, key]() {
        self->sigBondKeyReported.emit(addr, key);
    });
}

// ==================== JZBLEController ====================
JZ_OBJECT_IMPL(JZBLEController, JZObject)

JZBLEController::JZBLEController(JZObject *parent)
    :JZObject(parent),sigConnected(this), sigDisconnected(this), sigDiscoveryFinished(this), sigError(this), sigStateChanged(this), sigBondKeyReported(this)
{
}

JZBLEController::~JZBLEController()
{
    disconnectFromDevice();
}

JZBleDriver *JZBLEController::getDriver()
{
    return JZBleDriver::instance();
}

const JZBLEDeviceInfo &JZBLEController::deviceInfo() const
{
    return m_deviceInfo;
}

// === Central ===
void JZBLEController::connectToDevice(const JZBLEDeviceInfo &deviceInfo,
                                   const std::vector<JZBleDriver::JZBleBondKey> &keys)
{
    if (!deviceInfo.isValid()){
        jzWarning() << "JZBLEDeviceInfo is invaild";
        return;
    }
    m_deviceInfo = deviceInfo;
    
    auto drv = JZBleDriver::instance();
    m_state = Connecting;
    sigStateChanged.emit(m_state);
    drv->connectToDevice(this, keys);
}

void JZBLEController::disconnectFromDevice()
{
    if (m_deviceInfo.isValid())
    {
        auto *drv = getDriver();
        m_state = Unconnected;
        sigStateChanged.emit(m_state);
        drv->disconnectFromDevice(this);
    }
}

void JZBLEController::discoverServices()
{
    if (!m_deviceInfo.isValid()) return;
    auto *drv = getDriver();
    m_state = Discovering;
    sigStateChanged.emit(m_state);
    drv->discoverServices(m_deviceInfo.address(), this);
}

std::vector<JZBluetoothUuid> JZBLEController::services() const 
{ 
    return m_serviceList; 
}

JZBLEService *JZBLEController::createService(const JZBluetoothUuid &svcUuid)
{
    auto *s = new JZBLEService(svcUuid, this);
    s->setDriver(getDriver());
    s->setDeviceAddress(m_deviceInfo.address());
    return s;
}
