#include "JZBLEService.h"
#include "JZBleDriver.h"
#include "JZThread.h"

// ==================== JZBLEServicePrivate 静态分发 ====================
JZBLEServicePrivate::JZBLEServicePrivate(class JZBLEService *q, const JZBluetoothUuid &uuid)
    : q_ptr(q), serviceUuid(uuid) {}

JZLowEnergyCharacteristic JZBLEServicePrivate::findChar(const JZBluetoothUuid &uuid) const
{
    for (const auto &ch : charList) { if (ch.uuid() == uuid) return ch; }
    return JZLowEnergyCharacteristic();
}

void JZBLEServicePrivate::onCharacteristicsDiscovered(JZBLEServicePrivate *d, const char *addr, const char *svc)
{
    std::string a(addr), s(svc);
    runOnObject(d->q_ptr, [d, a, s]() {
        JZBleAbstractDriver *drv = JZBleAbstractDriver::instance();
        if (a == d->deviceAddress && s == d->serviceUuid.toString() && drv) {
            auto infos = drv->characteristics(a, s);
            d->charList.clear();
            for (auto &i : infos) {
                JZLowEnergyCharacteristic ch;
                ch.setUuid(i.uuid());
                ch.setProperties(i.properties());
                ch.setValue(i.value());
                d->charList.push_back(ch);
            }
            d->state = Discovered;
            d->q_ptr->sigStateChanged.emit(d->state);
        }
    });
}
void JZBLEServicePrivate::onCharacteristicRead(JZBLEServicePrivate *d, const char *addr, const char *svc, const char *val)
{
    std::string a(addr), s(svc), v(val ? val : "");
    runOnObject(d->q_ptr, [d, a, s, v]() {
        if (a == d->deviceAddress && s == d->serviceUuid.toString())
            d->q_ptr->sigCharacteristicRead.emit(d->findChar(JZBluetoothUuid(s)), v);
    });
}
void JZBLEServicePrivate::onCharacteristicWritten(JZBLEServicePrivate *d, const char *addr, const char *svc, const char *val, bool)
{
    std::string a(addr), s(svc), v(val ? val : "");
    runOnObject(d->q_ptr, [d, a, s, v]() {
        if (a == d->deviceAddress && s == d->serviceUuid.toString())
            d->q_ptr->sigCharacteristicWritten.emit(d->findChar(JZBluetoothUuid(s)), v);
    });
}
void JZBLEServicePrivate::onCharacteristicChanged(JZBLEServicePrivate *d, const char *addr, const char *svc, const char *val)
{
    std::string a(addr), s(svc), v(val ? val : "");
    runOnObject(d->q_ptr, [d, a, s, v]() {
        if (a == d->deviceAddress && s == d->serviceUuid.toString())
            d->q_ptr->sigCharacteristicChanged.emit(d->findChar(JZBluetoothUuid(s)), v);
    });
}
void JZBLEServicePrivate::onDescriptorRead(JZBLEServicePrivate *d, const char *, const char *, const char *, const char *val)
{
    std::string v(val ? val : "");
    runOnObject(d->q_ptr, [d, v]() {
        d->q_ptr->sigDescriptorRead.emit(JZLowEnergyDescriptor(), v);
    });
}
void JZBLEServicePrivate::onDescriptorWritten(JZBLEServicePrivate *d, const char *, const char *, const char *, const char *val, bool)
{
    std::string v(val ? val : "");
    runOnObject(d->q_ptr, [d, v]() {
        d->q_ptr->sigDescriptorWritten.emit(JZLowEnergyDescriptor(), v);
    });
}

// ==================== JZBLEService ====================
JZ_OBJECT_IMPL(JZBLEService, JZObject)

JZBLEService::JZBLEService(const JZBluetoothUuid &svc, JZObject *p)
    : JZObject(p),
      sigStateChanged(this), sigCharacteristicRead(this), sigCharacteristicWritten(this),
      sigCharacteristicChanged(this), sigDescriptorRead(this), sigDescriptorWritten(this), sigError(this),
      d(new JZBLEServicePrivate(this, svc)) {}
JZBLEService::~JZBLEService() { delete d; }

void JZBLEService::discoverDetails()
{
    if (d->driver && !d->deviceAddress.empty())
    {
        d->state = JZBLEServicePrivate::Discovering;
        sigStateChanged.emit(d->state);
        d->driver->discoverCharacteristics(d->deviceAddress, d->serviceUuid.toString(), d);
    }
}
void JZBLEService::readCharacteristic(const JZBluetoothUuid &cuuid)
{
    if (d->driver && !d->deviceAddress.empty())
        d->driver->readCharacteristic(d->deviceAddress, d->serviceUuid.toString(), cuuid.toString(), d);
}
void JZBLEService::writeCharacteristic(const JZBluetoothUuid &cuuid, const std::string &v, bool nrs)
{
    if (d->driver && !d->deviceAddress.empty())
        d->driver->writeCharacteristic(d->deviceAddress, d->serviceUuid.toString(), cuuid.toString(), v, nrs, d);
}
void JZBLEService::readDescriptor(const JZBluetoothUuid &cuuid, const JZBluetoothUuid &duuid)
{
    if (d->driver && !d->deviceAddress.empty())
        d->driver->readDescriptor(d->deviceAddress, d->serviceUuid.toString(), cuuid.toString(), duuid.toString(), d);
}
void JZBLEService::writeDescriptor(const JZBluetoothUuid &cuuid, const JZBluetoothUuid &duuid, const std::string &v)
{
    if (d->driver && !d->deviceAddress.empty())
        d->driver->writeDescriptor(d->deviceAddress, d->serviceUuid.toString(), cuuid.toString(), duuid.toString(), v, d);
}

JZBluetoothUuid JZBLEService::serviceUuid() const { return d->serviceUuid; }
std::vector<JZLowEnergyCharacteristic> JZBLEService::characteristics() const { return d->charList; }
JZLowEnergyCharacteristic JZBLEService::characteristic(const JZBluetoothUuid &u) const { return d->findChar(u); }
int JZBLEService::state() const { return d->state; }
int JZBLEService::error() const { return d->error; }
std::string JZBLEService::errorString() const { return d->errorMsg; }
void JZBLEService::setDriver(JZBleAbstractDriver *drv) { d->driver = drv; }
void JZBLEService::setDeviceAddress(const std::string &a) { d->deviceAddress = a; }
