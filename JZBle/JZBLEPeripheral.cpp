#include "JZBLEPeripheral.h"
#include "JZBleDriver.h"
#include "JZThread.h"

// ==================== JZBLEPeripheral 静态分发 ====================
JZBleDriver *JZBLEPeripheral::getDriver() { return JZBleDriver::instance(); }

void JZBLEPeripheral::onClientConnected(JZBLEPeripheral *self)
{
    runOnObject(self, [self]() {
        self->m_state = Connected;
        self->sigClientConnected.emit();
    });
}
void JZBLEPeripheral::onClientDisconnected(JZBLEPeripheral *self)
{
    runOnObject(self, [self]() {
        self->m_state = Unconnected;
        self->sigClientDisconnected.emit();
    });
}
void JZBLEPeripheral::onCharWritten(JZBLEPeripheral *self, const char *charUuid, const char *value)
{
    std::string c(charUuid), v(value ? value : "");
    runOnObject(self, [self, c, v]() { self->sigCharacteristicWritten.emit(c, v); });
}

// ==================== JZBLEPeripheral ====================
JZ_OBJECT_IMPL(JZBLEPeripheral, JZObject)

JZBLEPeripheral::JZBLEPeripheral(JZObject *parent)
    : JZObject(parent),
      sigClientConnected(this), sigClientDisconnected(this), sigCharacteristicWritten(this)
{
}

JZBLEPeripheral::~JZBLEPeripheral()
{
    stopAdvertising();
}

int JZBLEPeripheral::state() const { return m_state; }
int JZBLEPeripheral::error() const { return m_error; }
std::string JZBLEPeripheral::errorString() const { return m_errorMsg; }

// === Peripheral ===
void JZBLEPeripheral::addService(const JZBluetoothUuid &serviceUuid)
{
    m_serviceList.push_back(serviceUuid);
}

void JZBLEPeripheral::addCharacteristic(const JZBluetoothUuid &serviceUuid, const JZBluetoothUuid &charUuid,
                                         int properties, const std::string &initialValue)
{
    (void)initialValue;
    if (auto *drv = getDriver())
        drv->addPeripheralCharacteristic(serviceUuid.toString(), charUuid.toString(), properties);
}

void JZBLEPeripheral::startAdvertising(const std::string &localName)
{
    if (auto *drv = getDriver())
    {
        drv->startAdvertising(localName.empty() ? "JZBle" : localName, m_serviceList, this);
        m_advertising = true;
    }
}

void JZBLEPeripheral::stopAdvertising()
{
    if (m_advertising)
    {
        if (auto *drv = getDriver())
            drv->stopAdvertising();
        m_advertising = false;
    }
}

bool JZBLEPeripheral::isAdvertising() const { return m_advertising; }

void JZBLEPeripheral::sendNotification(const JZBluetoothUuid &charUuid, const std::string &value)
{
    if (auto *drv = getDriver())
        drv->peripheralSendNotify(charUuid.toString(), value);
}
