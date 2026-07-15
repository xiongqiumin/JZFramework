#include "JZBleDriver.h"
#include "JZBLEDeviceDiscoveryAgent.h"
#include "JZBLECentral.h"
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

void JZBleDriver::registerAgent(JZBLEDeviceDiscoveryAgentPrivate *d)
{ 
    if (!vec_contains(m_agents, d)) 
        m_agents.push_back(d); 
}

void JZBleDriver::unregisterAgent(JZBLEDeviceDiscoveryAgentPrivate *d)
{ 
    vec_removeAll(m_agents, d); 
}

// ==================== 事件转发中转 ====================
// JZBleDriver 是 JZBLECentral / JZBLEPeripheral 的 friend, 这里把受保护的
// 静态分发方法封装为公开的转发入口, 具体驱动只调用本层, 不直接碰 friend 关系。
void JZBleDriver::centralOnDeviceConnected(JZBLECentral *self, const char *addr)
{ JZBLECentral::onDeviceConnected(self, addr); }

void JZBleDriver::centralOnDeviceDisconnected(JZBLECentral *self, const char *addr)
{ JZBLECentral::onDeviceDisconnected(self, addr); }

void JZBleDriver::centralOnServicesDiscovered(JZBLECentral *self, const char *addr)
{ JZBLECentral::onServicesDiscovered(self, addr); }

void JZBleDriver::centralOnError(JZBLECentral *self, int code, const char *msg)
{ JZBLECentral::onError(self, code, msg); }

void JZBleDriver::centralOnBondKeyReported(JZBLECentral *self, const std::string &addr, const JZBleBondKey &key)
{ JZBLECentral::onBondKeyReported(self, addr, key); }

void JZBleDriver::peripheralOnClientConnected(JZBLEPeripheral *self)
{ JZBLEPeripheral::onClientConnected(self); }

void JZBleDriver::peripheralOnClientDisconnected(JZBLEPeripheral *self)
{ JZBLEPeripheral::onClientDisconnected(self); }

void JZBleDriver::peripheralOnCharWritten(JZBLEPeripheral *self, const char *charUuid, const char *value)
{ JZBLEPeripheral::onCharWritten(self, charUuid, value); }
