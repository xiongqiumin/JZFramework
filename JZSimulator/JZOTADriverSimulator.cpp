#include "JZOTADriverSimulator.h"
#include "JZSimPack.h"

JZ_OBJECT_IMPL(JZOTADriverSimulator, JZObject)

JZOTADriverSimulator *JZOTADriverSimulator::simInstance()
{
    return dynamic_cast<JZOTADriverSimulator *>(JZOTADriver::instance());
}

JZOTADriverSimulator::JZOTADriverSimulator()
{
    m_sim = JZSimServer::instance();
    m_simToken = m_sim->registerSimulator(
        [this](JZSimCmd cmd) { return support(cmd); },
        [this](const JZSimPack &pack) { onRecv(pack); });
    m_sim->start("ota");
}

JZOTADriverSimulator::~JZOTADriverSimulator()
{
    if (m_sim)
    {
        m_sim->unregisterSimulator(m_simToken);
        m_sim->stop("ota");
    }
}

bool JZOTADriverSimulator::support(JZSimCmd cmd) const
{
    int c = (int)cmd;
    return c >= 300 && c < 400; // SIM_OTA_CMD_BASE .. SIM_OTA_CMD_BASE+100
}

void JZOTADriverSimulator::onRecv(const JZSimPack &pack)
{
    (void)pack;
}

void JZOTADriverSimulator::sendSim(JZSimCmd cmd, const JZVariantMap &m)
{
    sigSim.emit(cmd, m);
    JZSimServer::instance()->sendSim(cmd, m);
}

JZResult<void> JZOTADriverSimulator::verify(const JZOTAUpdateInfo &info)
{
    (void)info;
    return JZResult<void>();
}

int64_t JZOTADriverSimulator::totalSize()
{
    return -1;
}

bool JZOTADriverSimulator::begin(int64_t totalSize)
{
    (void)totalSize;
    JZVariantMap m; m["total"] = JZVariant((int)totalSize);
    sigSim.emit(SIM_OTA_CMD_BASE, m);
    JZSimServer::instance()->addAssoc("ota", "update", "OTA update");
    return false;
}

int64_t JZOTADriverSimulator::write(int64_t offset, const JZByteArray &buffer)
{
    (void)offset;
    (void)buffer;
    return -1;
}

bool JZOTADriverSimulator::finish()
{
    JZSimServer::instance()->removeAssoc("ota", "update");
    return false;
}

void JZOTADriverSimulator::cancel()
{
    JZSimServer::instance()->removeAssoc("ota", "update");
}

void JZOTADriverSimulator::reboot()
{
}
