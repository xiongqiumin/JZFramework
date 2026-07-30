#include "JZBleDriver.h"
#include "JZBLEDeviceDiscoveryAgent.h"
#include "JZBLEController.h"
#include "JZBLEPeripheral.h"
#include "JZStd.h"
#include "JZDebug.h"

JZBleDriver *JZBleDriver::s_instance = nullptr;

JZBleDriver *JZBleDriver::instance() 
{
    JZ_ASSERT_X(s_instance,"JZBleDriver not init");
    return s_instance; 
}

void JZBleDriver::setInstance(JZBleDriver *inst) 
{ 
    if (s_instance && s_instance != inst) 
    {
        if (inst)
            jzWarning("JZBleDriver instance already set, replacing old driver");
        delete s_instance;
    }
    s_instance = inst; 
}

// ==================== 事件转发中转 ====================
// JZBleDriver 是 JZBLEController / JZBLEPeripheral 的 friend, 这里把受保护的
// 静态分发方法封装为公开的转发入口, 具体驱动只调用本层, 不直接碰 friend 关系。
void JZBleDriver::centralOnDeviceConnected(JZBLEController *self, const char *addr)
{ JZBLEController::onDeviceConnected(self, addr); }

void JZBleDriver::centralOnDeviceDisconnected(JZBLEController *self, const char *addr)
{ JZBLEController::onDeviceDisconnected(self, addr); }

void JZBleDriver::centralOnServicesDiscovered(JZBLEController *self, const char *addr)
{ JZBLEController::onServicesDiscovered(self, addr); }

void JZBleDriver::centralOnError(JZBLEController *self, int code, const char *msg)
{ JZBLEController::onError(self, code, msg); }

void JZBleDriver::centralOnBondKeyReported(JZBLEController *self, const std::string &addr, const JZBleBondKey &key)
{ JZBLEController::onBondKeyReported(self, addr, key); }

void JZBleDriver::peripheralOnClientConnected(JZBLEPeripheral *self, std::shared_ptr<JZBLEPeripheralConnect> connect)
{ JZBLEPeripheral::onClientConnected(self, connect); }

void JZBleDriver::peripheralOnClientDisconnected(JZBLEPeripheral *self, std::shared_ptr<JZBLEPeripheralConnect> connect)
{ JZBLEPeripheral::onClientDisconnected(self, connect); }

void JZBleDriver::peripheralOnCharWritten(JZBLEPeripheral *self, std::shared_ptr<JZBLEPeripheralConnect> connect,
                                          const JZBluetoothUuid &charUuid, const JZByteArray &value)
{ JZBLEPeripheral::onCharWritten(self, connect, charUuid, value); }
