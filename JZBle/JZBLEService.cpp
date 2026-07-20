#include "JZBLEService.h"
#include "JZBleDriver.h"
#include "JZThread.h"

// ==================== JZBLEService 静态分发 ====================
JZLowEnergyCharacteristic JZBLEService::findChar(const JZBluetoothUuid &uuid) const
{
    for (const auto &ch : charList) { if (ch.uuid() == uuid) return ch; }
    return JZLowEnergyCharacteristic();
}

void JZBLEService::onCharacteristicsDiscovered(JZBLEService *d, const char *addr, const char *svc)
{
    std::string a(addr), s(svc);
    runOnObject(d, [d, a, s]() {
        JZBleDriver *drv = JZBleDriver::instance();
        if (a == d->deviceAddress && s == d->m_serviceUuid.toString()) {
            auto infos = drv->characteristics(a, s);
            d->charList.clear();
            for (auto &i : infos) {
                JZLowEnergyCharacteristic ch;
                ch.setUuid(i.uuid());
                ch.setProperties(i.properties());
                ch.setValue(i.value());
                d->charList.push_back(ch);
            }
            d->m_state = Discovered;
            d->sigStateChanged.emit(d->m_state);
        }
    });
}
void JZBLEService::onCharacteristicRead(JZBLEService *d, const char *addr, const char *svc, const JZByteArray &val)
{
    std::string a(addr), s(svc);
    runOnObject(d, [d, a, s, val]() {
        if (a == d->deviceAddress && s == d->m_serviceUuid.toString())
            d->sigCharacteristicRead.emit(d->findChar(JZBluetoothUuid(s)), val);
    });
}
void JZBLEService::onCharacteristicWritten(JZBLEService *d, const char *addr, const char *svc, const JZByteArray &val, bool)
{
    std::string a(addr), s(svc);
    runOnObject(d, [d, a, s, val]() {
        if (a == d->deviceAddress && s == d->m_serviceUuid.toString())
            d->sigCharacteristicWritten.emit(d->findChar(JZBluetoothUuid(s)), val);
    });
}
void JZBLEService::onCharacteristicChanged(JZBLEService *d, const char *addr, const char *svc, const JZByteArray &val)
{
    std::string a(addr), s(svc);
    runOnObject(d, [d, a, s, val]() {
        if (a == d->deviceAddress && s == d->m_serviceUuid.toString())
            d->sigCharacteristicChanged.emit(d->findChar(JZBluetoothUuid(s)), val);
    });
}
void JZBLEService::onDescriptorRead(JZBLEService *d, const char *, const char *, const char *, const JZByteArray &val)
{
    runOnObject(d, [d, val]() {
        d->sigDescriptorRead.emit(JZLowEnergyDescriptor(), val);
    });
}
void JZBLEService::onDescriptorWritten(JZBLEService *d, const char *, const char *, const char *, const JZByteArray &val, bool)
{
    runOnObject(d, [d, val]() {
        d->sigDescriptorWritten.emit(JZLowEnergyDescriptor(), val);
    });
}

// ==================== JZBLEService ====================
JZ_OBJECT_IMPL(JZBLEService, JZObject)

JZBLEService::JZBLEService(const JZBluetoothUuid &svc, JZObject *p)
    : JZObject(p),
      m_serviceUuid(svc),
      sigStateChanged(this), sigCharacteristicRead(this), sigCharacteristicWritten(this),
      sigCharacteristicChanged(this), sigDescriptorRead(this), sigDescriptorWritten(this), sigError(this) {}
JZBLEService::~JZBLEService() {}

void JZBLEService::discoverDetails()
{
    if (driver && !deviceAddress.empty())
    {
        m_state = Discovering;
        sigStateChanged.emit(m_state);
        driver->discoverCharacteristics(deviceAddress, m_serviceUuid.toString(), this);
    }
}
void JZBLEService::readCharacteristic(const JZBluetoothUuid &cuuid)
{
    if (driver && !deviceAddress.empty())
        driver->readCharacteristic(deviceAddress, m_serviceUuid.toString(), cuuid.toString(), this);
}
void JZBLEService::writeCharacteristic(const JZBluetoothUuid &cuuid, const JZByteArray &v, bool nrs)
{
    if (driver && !deviceAddress.empty())
        driver->writeCharacteristic(deviceAddress, m_serviceUuid.toString(), cuuid.toString(), v, nrs, this);
}
void JZBLEService::readDescriptor(const JZBluetoothUuid &cuuid, const JZBluetoothUuid &duuid)
{
    if (driver && !deviceAddress.empty())
        driver->readDescriptor(deviceAddress, m_serviceUuid.toString(), cuuid.toString(), duuid.toString(), this);
}
void JZBLEService::writeDescriptor(const JZBluetoothUuid &cuuid, const JZBluetoothUuid &duuid, const JZByteArray &v)
{
    if (driver && !deviceAddress.empty())
        driver->writeDescriptor(deviceAddress, m_serviceUuid.toString(), cuuid.toString(), duuid.toString(), v, this);
}

std::vector<JZLowEnergyCharacteristic> JZBLEService::characteristics() const { return charList; }
JZLowEnergyCharacteristic JZBLEService::characteristic(const JZBluetoothUuid &u) const { return findChar(u); }
int JZBLEService::state() const { return m_state; }
int JZBLEService::error() const { return m_error; }
std::string JZBLEService::errorString() const { return errorMsg; }
void JZBLEService::setDriver(JZBleDriver *drv) { driver = drv; }
void JZBLEService::setDeviceAddress(const std::string &a) { deviceAddress = a; }
