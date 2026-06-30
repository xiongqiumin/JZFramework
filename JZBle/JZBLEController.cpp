#include "JZBLEController.h"
#include "JZBLEService.h"
#include "JZBleDriver.h"
#include "JZThread.h"

// ==================== JZBLEControllerPrivate 静态分发 ====================
JZBLEControllerPrivate::JZBLEControllerPrivate(class JZBLEController *q) : q_ptr(q) {}

JZBleAbstractDriver *JZBLEControllerPrivate::getDriver() { return JZBleAbstractDriver::instance(); }

void JZBLEControllerPrivate::onDeviceDiscovered(JZBLEControllerPrivate *d, const JZBluetoothDeviceInfo &info)
{
    JZBluetoothDeviceInfo c = info;
    runOnObject(d->q_ptr, [d, c]() { d->q_ptr->sigDeviceDiscovered.emit(c); });
}
void JZBLEControllerPrivate::onScanFinished(JZBLEControllerPrivate *d)
{
    runOnObject(d->q_ptr, [d]() { d->q_ptr->sigScanFinished.emit(); });
}
void JZBLEControllerPrivate::onDeviceConnected(JZBLEControllerPrivate *d, const char *addr)
{
    std::string a(addr);
    runOnObject(d->q_ptr, [d, a]() {
        if (a == d->deviceInfo.address()) {
            d->state = JZBLEControllerPrivate::Connected;
            d->q_ptr->sigConnected.emit();
            d->q_ptr->sigStateChanged.emit(d->state);
        }
    });
}
void JZBLEControllerPrivate::onDeviceDisconnected(JZBLEControllerPrivate *d, const char *addr)
{
    std::string a(addr);
    runOnObject(d->q_ptr, [d, a]() {
        if (a == d->deviceInfo.address()) {
            d->state = JZBLEControllerPrivate::Unconnected;
            d->q_ptr->sigDisconnected.emit();
            d->q_ptr->sigStateChanged.emit(d->state);
        }
    });
}
void JZBLEControllerPrivate::onServicesDiscovered(JZBLEControllerPrivate *d, const char *addr)
{
    std::string a(addr);
    runOnObject(d->q_ptr, [d, a]() {
        JZBleAbstractDriver *drv = JZBleAbstractDriver::instance();
        if (a == d->deviceInfo.address() && drv) {
            d->serviceList = drv->services(a);
            d->state = JZBLEControllerPrivate::Connected;
            d->q_ptr->sigDiscoveryFinished.emit();
            d->q_ptr->sigStateChanged.emit(d->state);
        }
    });
}
void JZBLEControllerPrivate::onError(JZBLEControllerPrivate *d, int code, const char *msg)
{
    std::string m(msg ? msg : "");
    runOnObject(d->q_ptr, [d, code, m]() {
        d->error = JZBLEControllerPrivate::UnknownError;
        d->errorMsg = m;
        d->q_ptr->sigError.emit(d->error);
    });
}
void JZBLEControllerPrivate::onClientConnected(JZBLEControllerPrivate *d)
{
    runOnObject(d->q_ptr, [d]() { d->q_ptr->sigClientConnected.emit(); });
}
void JZBLEControllerPrivate::onClientDisconnected(JZBLEControllerPrivate *d)
{
    runOnObject(d->q_ptr, [d]() { d->q_ptr->sigClientDisconnected.emit(); });
}
void JZBLEControllerPrivate::onCharWritten(JZBLEControllerPrivate *d, const char *charUuid, const char *value)
{
    std::string c(charUuid), v(value ? value : "");
    runOnObject(d->q_ptr, [d, c, v]() { d->q_ptr->sigCharacteristicWritten.emit(c, v); });
}

// ==================== JZBLEController ====================
JZ_OBJECT_IMPL(JZBLEController, JZObject)

JZBLEController *JZBLEController::createCentral(const JZBluetoothDeviceInfo &deviceInfo, JZObject *parent)
{
    auto *c = new JZBLEController(parent);
    c->setDeviceInfo(deviceInfo);
    return c;
}
JZBLEController *JZBLEController::createPeripheral(JZObject *parent)
{
    return new JZBLEController(parent);
}

JZBLEController::JZBLEController(JZObject *parent)
    : JZObject(parent),
      sigDeviceDiscovered(this), sigScanFinished(this), sigConnected(this),
      sigDisconnected(this), sigDiscoveryFinished(this), sigStateChanged(this), sigError(this),
      sigClientConnected(this), sigClientDisconnected(this),
      sigCharacteristicWritten(this),
      d(new JZBLEControllerPrivate(this))
{
    // 默认 CentralRole（可通过 setDeviceInfo 设置设备后使用）
    if (JZBleAbstractDriver *drv = d->getDriver())
        drv->registerController(d);
}
JZBLEController::~JZBLEController()
{
    disconnectFromDevice();
    if (auto *drv = d->getDriver()) drv->unregisterController(d);
    delete d;
}

JZBLEController::ControllerRole JZBLEController::role() const { return d->deviceInfo.isValid() ? CentralRole : PeripheralRole; }
int JZBLEController::state() const { return d->state; }
int JZBLEController::error() const { return d->error; }
std::string JZBLEController::errorString() const { return d->errorMsg; }

JZBluetoothDeviceInfo JZBLEController::deviceInfo() const { return d->deviceInfo; }
void JZBLEController::setDeviceInfo(const JZBluetoothDeviceInfo &info) { d->deviceInfo = info; }

// === Central ===
void JZBLEController::connectToDevice()
{
    if (!d->deviceInfo.isValid()) return;
    if (auto *drv = d->getDriver())
    {
        d->state = JZBLEControllerPrivate::Connecting;
        sigStateChanged.emit(d->state);
        drv->connectToDevice(d->deviceInfo.address(), d);
    }
}
void JZBLEController::disconnectFromDevice()
{
    if (auto *drv = d->getDriver())
    {
        if (d->deviceInfo.isValid())
        {
            d->state = JZBLEControllerPrivate::Disconnecting;
            sigStateChanged.emit(d->state);
            drv->disconnectFromDevice(d->deviceInfo.address(), d);
        }
    }
}
void JZBLEController::discoverServices()
{
    if (auto *drv = d->getDriver())
    {
        if (!d->deviceInfo.isValid()) return;
        d->state = JZBLEControllerPrivate::Discovering;
        sigStateChanged.emit(d->state);
        drv->discoverServices(d->deviceInfo.address(), d);
    }
}
std::vector<JZBluetoothUuid> JZBLEController::services() const { return d->serviceList; }
JZBLEService *JZBLEController::createServiceObject(const JZBluetoothUuid &svcUuid, JZObject *parent)
{
    auto *s = new JZBLEService(svcUuid, parent ? parent : this);
    s->setDriver(d->getDriver());
    s->setDeviceAddress(d->deviceInfo.address());
    return s;
}

// === Peripheral ===
void JZBLEController::addService(const JZBluetoothUuid &serviceUuid)
{
    d->serviceList.push_back(serviceUuid);
}
void JZBLEController::addCharacteristic(const JZBluetoothUuid &serviceUuid, const JZBluetoothUuid &charUuid,
                                         int properties, const std::string &initialValue)
{
    (void)initialValue;
    if (auto *drv = d->getDriver())
        drv->addPeripheralCharacteristic(serviceUuid.toString(), charUuid.toString(), properties);
}
void JZBLEController::startAdvertising(const std::string &localName)
{
    if (auto *drv = d->getDriver())
        drv->startAdvertising(localName.empty() ? "JZBle" : localName, d->serviceList, d);
}
void JZBLEController::stopAdvertising()
{
    if (auto *drv = d->getDriver())
        drv->stopAdvertising();
}
bool JZBLEController::isAdvertising() const { return false; }
