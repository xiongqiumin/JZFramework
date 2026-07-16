#include "JZSensorDriver.h"
#include "JZDebug.h"

std::vector<JZSensorDriver*> JZSensorDriver::s_instances;

void JZSensorDriver::addInstance(JZSensorDriver *inst)
{
    if (!inst)
        return;

    // 检查重复注册
    for (auto *d : s_instances)
    {
        if (d == inst)
        {
            jzWarning("JZSensorDriver: instance already registered, skipping");
            return;
        }
    }
    s_instances.push_back(inst);
}

void JZSensorDriver::clearInstances()
{
    for (auto *d : s_instances)
        delete d;
    s_instances.clear();
}

std::vector<JZSensorDriver*> JZSensorDriver::instances()
{
    return s_instances;
}

JZSensorDriver *JZSensorDriver::findDriver(const std::string &id)
{
    for (auto *d : s_instances)
    {
        if (d->supportsId(id))
            return d;
    }
    return nullptr;
}

std::vector<JZSensorInfo> JZSensorDriver::allAvailableSensors()
{
    std::vector<JZSensorInfo> result;
    for (auto *d : s_instances)
    {
        auto list = d->availableSensors();
        result.insert(result.end(), list.begin(), list.end());
    }
    return result;
}
