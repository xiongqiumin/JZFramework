#include "JZSensorDriverSimulator.h"
#include "JZSimPack.h"

JZSensorDriverSimulator *JZSensorDriverSimulator::s_inst = nullptr;

JZSensorDriverSimulator::JZSensorDriverSimulator()
{
    if (!s_inst)
        s_inst = this;
}

JZSensorDriverSimulator::~JZSensorDriverSimulator()
{
    if (s_inst == this)
        s_inst = nullptr;
}

JZSensorDriverSimulator *JZSensorDriverSimulator::simInstance()
{
    return s_inst;
}

void JZSensorDriverSimulator::onRecv(JZSimPack *sp)
{
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
    (void)sensor;
    return false;
}

void JZSensorDriverSimulator::close(JZSensor *sensor)
{
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
    return 0.0 / 0.0; // NaN
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
