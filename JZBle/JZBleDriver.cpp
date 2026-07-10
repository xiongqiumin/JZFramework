#include "JZBleDriver.h"
#include "JZBLEDeviceDiscoveryAgent.h"
#include "JZStd.h"

JZBleDriver *JZBleDriver::s_instance = nullptr;

JZBleDriver *JZBleDriver::instance() 
{
    JZ_ASSERT_X(s_instance,"JZBleDriver not init");
    return s_instance; 
}
void JZBleDriver::setInstance(JZBleDriver *inst) { s_instance = inst; }

void JZBleDriver::registerAgent(JZBLEDeviceDiscoveryAgentPrivate *d)
{ 
    if (!vec_contains(m_agents, d)) 
        m_agents.push_back(d); 
}

void JZBleDriver::unregisterAgent(JZBLEDeviceDiscoveryAgentPrivate *d)
{ 
    vec_removeAll(m_agents, d); 
}
