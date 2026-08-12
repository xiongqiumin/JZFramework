#include "JZSimulatorClient.h"
#include "JZApplication.h"

#if HAS_JZSensor
#include "JZSensorDriverSimulator.h"
#endif

#if HAS_JZOTA
#include "JZOTADriverSimulator.h"
#endif

#if HAS_JZWifi
#include "JZWifiDriverSimulator.h"
#endif

#if HAS_JZGpio
#include "JZGpioDriverSimulator.h"    
#endif

#if HAS_JZBle
#include "JZBleDriverSimulator.h"        
#endif      

JZ_OBJECT_IMPL(JZSimulatorClient, JZObject)

JZSimulatorClient *JZSimulatorClient::m_inst = nullptr;

JZSimulatorClient *JZSimulatorClient::instance()
{
    if (!m_inst)
        m_inst = new JZSimulatorClient();
    return m_inst;
}

void JZSimulatorClient::setInstance(JZSimulatorClient *inst)
{
    m_inst = inst;
}

JZSimulatorClient::JZSimulatorClient(const std::string &ip, int port)
    : JZObject()
{
    JZSimPack::ensureRegistered();

    m_client = new JZNetClient(this);
    connect(&m_client->sigPacketReceive, [this](JZNetPackPtr p) { onClientPacket(p); });
    //m_client->connectToHost(ip, port);    
}

JZSimulatorClient::~JZSimulatorClient()
{
    if (m_client)
    {
        delete m_client;
        m_client = nullptr;
    }
}

bool JZSimulatorClient::isConnected() const
{
    return m_client && m_client->isConnect();
}

void JZSimulatorClient::sendSim(JZSimCmd cmd, const JZVariantMap &data)
{
    if (!isConnected())
        return;
    JZSimPack pack;
    pack.cmd = cmd;
    pack.data = data;
    m_client->sendPacket(&pack);
}

void JZSimulatorClient::onClientPacket(JZNetPackPtr pack)
{
    JZ_ASSERT(pack->packetType() == JZSimPack::PackType);

    auto *sp = (JZSimPack *)(pack.get());

    // 按模块段路由: 直接调用对应驱动单例的 onRecv() (单播, 而非广播给所有驱动)。
    // 各可选模块用 HAS_JZxxx 宏保护(由 JZFramework.cmake 按模块是否启用定义),
    // 模块未启用时该段被编译为空, 避免链接到未编译进来的驱动类。
    if (sp->cmd >= SIM_SENSOR_CMD_BASE)
    {
#if HAS_JZSensor
        if (auto *d = JZSensorDriverSimulator::simInstance()) d->onRecv(sp);
#endif
    }
    else if (sp->cmd >= SIM_OTA_CMD_BASE)
    {
#if HAS_JZOTA
        if (auto *d = JZOTADriverSimulator::simInstance()) d->onRecv(sp);
#endif
    }
    else if (sp->cmd >= SIM_WIFI_AP_START)
    {
#if HAS_JZWifi
        if (auto *d = JZWifiDriverSimulator::simInstance()) d->onRecv(sp);
#endif
    }
    else if (sp->cmd >= SIM_GPIO_CMD_OPEN)
    {
#if HAS_JZGpio
        if (auto *d = JZGpioDriverSimulator::simInstance()) d->onRecv(sp);
#endif
    }
    else
    {
#if HAS_JZBle        
        // BLE 模块始终存在, 无需条件编译
        if (auto *d = JZBleDriverSimulator::simInstance()) d->onRecv(sp);
#endif        
    }
}
