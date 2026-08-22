#include "JZSensorDriverSimulator.h"
#include "JZSimPack.h"
#include "JZSensor.h"
#include <cmath>
#include <limits>

JZ_OBJECT_IMPL(JZSensorDriverSimulator, JZObject)

JZSensorDriverSimulator *JZSensorDriverSimulator::s_inst = nullptr;

JZSensorDriverSimulator::JZSensorDriverSimulator()
{
    if (!s_inst)
        s_inst = this;
    m_sim = JZSimServer::instance();
    m_simToken = m_sim->registerSimulator(
        [this](JZSimCmd cmd) { return support(cmd); },
        [this](const JZSimPack &pack) { onRecv(pack); });
    m_sim->start("sensor");
}

JZSensorDriverSimulator::~JZSensorDriverSimulator()
{
    if (m_sim)
    {
        m_sim->unregisterSimulator(m_simToken);
        m_sim->stop("sensor");
    }
    if (s_inst == this)
        s_inst = nullptr;
}

JZSensorDriverSimulator *JZSensorDriverSimulator::simInstance()
{
    return s_inst;
}

bool JZSensorDriverSimulator::support(JZSimCmd cmd) const
{
    int c = (int)cmd;
    return c >= 400 && c < 500; // SIM_SENSOR_CMD_BASE .. SIM_SENSOR_CMD_BASE+100
}

void JZSensorDriverSimulator::onRecv(const JZSimPack &pack)
{
    (void)pack;
}

void JZSensorDriverSimulator::sendSim(JZSimCmd cmd, const JZVariantMap &m)
{
    sigSim.emit(cmd, m);
    JZSimServer::instance()->sendSim(cmd, m);
}

bool JZSensorDriverSimulator::supportsId(const std::string &id)
{
    (void)id;
    return false;
}

std::vector<JZSensorInfo> JZSensorDriverSimulator::availableSensors()
{
    return {};
}

bool JZSensorDriverSimulator::open(JZSensor *sensor)
{
    std::string id = sensor ? sensor->objectName() : std::string();
    JZVariantMap m; m["id"] = JZVariant(id);
    sigSim.emit(SIM_SENSOR_CMD_BASE, m);
    JZSimServer::instance()->addAssoc("sensor", id.empty() ? "sensor" : id,
                                      "Sensor " + (id.empty() ? std::string("(unknown)") : id));
    (void)sensor;
    return false;
}

void JZSensorDriverSimulator::close(JZSensor *sensor)
{
    std::string id = sensor ? sensor->objectName() : std::string();
    JZSimServer::instance()->removeAssoc("sensor", id.empty() ? "sensor" : id);
    (void)sensor;
}

bool JZSensorDriverSimulator::isOpen(JZSensor *sensor)
{
    (void)sensor;
    return false;
}

double JZSensorDriverSimulator::read(JZSensor *sensor)
{
    (void)sensor;
    return std::numeric_limits<double>::quiet_NaN(); // NaN
}

bool JZSensorDriverSimulator::startContinuous(JZSensor *sensor, int intervalMs)
{
    (void)sensor;
    (void)intervalMs;
    return false;
}

void JZSensorDriverSimulator::stopContinuous(JZSensor *sensor)
{
    (void)sensor;
}
