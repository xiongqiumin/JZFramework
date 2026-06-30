#include "JZBleDriver.h"
#include "JZBLEController.h"
#include "JZBLEDeviceDiscoveryAgent.h"
#include "JZBLEService.h"

JZBleAbstractDriver *JZBleAbstractDriver::s_instance = nullptr;

JZBleAbstractDriver *JZBleAbstractDriver::instance() { return s_instance; }
void JZBleAbstractDriver::setInstance(JZBleAbstractDriver *inst) { s_instance = inst; }

void JZBleAbstractDriver::registerController(JZBLEControllerPrivate *d)
    { if (!vec_contains(m_controllers, d)) m_controllers.push_back(d); }
void JZBleAbstractDriver::unregisterController(JZBLEControllerPrivate *d)
    { vec_remove(m_controllers, d); }

void JZBleAbstractDriver::registerAgent(JZBLEDeviceDiscoveryAgentPrivate *d)
    { if (!vec_contains(m_agents, d)) m_agents.push_back(d); }
void JZBleAbstractDriver::unregisterAgent(JZBLEDeviceDiscoveryAgentPrivate *d)
    { vec_remove(m_agents, d); }
