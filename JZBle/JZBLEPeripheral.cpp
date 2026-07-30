#include "JZBLEPeripheral.h"
#include "JZBleDriver.h"
#include "JZThread.h"

// ==================== JZBLEPeripheral 静态分发 ====================
JZBleDriver *JZBLEPeripheral::getDriver() { return JZBleDriver::instance(); }

void JZBLEPeripheral::onClientConnected(JZBLEPeripheral *self, std::shared_ptr<JZBLEPeripheralConnect> connect)
{
    runOnObject(self, [self, connect]() {
        self->m_connects.push_back(connect);
        self->sigConnected.emit(connect.get());
    });
}
void JZBLEPeripheral::onClientDisconnected(JZBLEPeripheral *self, std::shared_ptr<JZBLEPeripheralConnect> connect)
{
    runOnObject(self, [self, connect]() {
        for (int i = 0; i < (int)self->m_connects.size(); i++) {
            if (self->m_connects[i].get() == connect.get()) {
                self->m_connects.erase(self->m_connects.begin() + i);
                break;
            }
        }
        self->sigDisconnected.emit(connect.get());
    });
}
void JZBLEPeripheral::onCharWritten(JZBLEPeripheral *self, std::shared_ptr<JZBLEPeripheralConnect> connect, const JZBluetoothUuid &charUuid, const JZByteArray &value)
{
    runOnObject(self, [self, connect, charUuid, value]() { 
        self->sigCharacteristicWritten.emit(connect.get(), charUuid, value); 
    });
}

// ==================== JZBLEPeripheral ====================
JZ_OBJECT_IMPL(JZBLEPeripheral, JZObject)

JZBLEPeripheral::JZBLEPeripheral(JZObject *parent)
    : JZObject(parent),
      sigConnected(this), sigDisconnected(this), sigCharacteristicWritten(this)
{
}

JZBLEPeripheral::~JZBLEPeripheral()
{
    stopAdvertising();

    auto connects = m_connects;
    for(auto connect : connects)
        disconnectFromDevice(connect.get());
}

void JZBLEPeripheral::disconnectFromDevice(JZBLEPeripheralConnect *connect)
{
    int con_idx = -1;
    for(int i = 0; i < (int)m_connects.size(); i++)
    {
        if(m_connects[i].get() == connect)
        {
            con_idx = i;
            break;
        }
    }
    if(con_idx == -1)
        return;

    auto *drv = getDriver();
    drv->peripheralDisconnect(this, connect);
    sigDisconnected.emit(connect);   
}

// === Peripheral ===
void JZBLEPeripheral::addService(const JZBleServiceData &serviceUuid)
{
    m_serviceList.push_back(serviceUuid);
}

void JZBLEPeripheral::startAdvertising()
{
    auto *drv = getDriver();
    drv->startAdvertising(this);
    m_advertising = true;
}

void JZBLEPeripheral::stopAdvertising()
{
    if (m_advertising)
    {
        getDriver()->stopAdvertising(this);
        m_advertising = false;
    }
}

bool JZBLEPeripheral::isAdvertising() const { return m_advertising; }

void JZBLEPeripheral::sendNotify(JZBLEPeripheralConnect *connect, const JZBluetoothUuid &charUuid, const JZByteArray &value)
{
    getDriver()->peripheralSendNotify(this, connect, charUuid, value);
}

void JZBLEPeripheral::sendNotifyToAll(const JZBluetoothUuid &charUuid, const JZByteArray &value)
{
    for(auto &c : m_connects)
        getDriver()->peripheralSendNotify(this, c.get(), charUuid, value);
}
