#include "JZGpioDriver.h"
#include "JZDebug.h"

std::vector<JZGpioDriver*> JZGpioDriver::s_instances;

void JZGpioDriver::addInstance(JZGpioDriver *inst)
{
    if (!inst)
        return;

    // 检查重复注册
    for (auto *d : s_instances)
    {
        if (d == inst)
        {
            jzWarning("JZGpioDriver: instance already registered, skipping");
            return;
        }
    }
    s_instances.push_back(inst);
}

void JZGpioDriver::clearInstances()
{
    for (auto *d : s_instances)
        delete d;
    s_instances.clear();
}

std::vector<JZGpioDriver*> JZGpioDriver::instances()
{
    return s_instances;
}

JZGpioDriver *JZGpioDriver::findDriver(int pin)
{
    for (auto *d : s_instances)
    {
        if (d->supportsPin(pin))
            return d;
    }
    return nullptr;
}

std::vector<JZGpioInfo> JZGpioDriver::allAvailableGpios()
{
    std::vector<JZGpioInfo> result;
    for (auto *d : s_instances)
    {
        auto list = d->availableGpios();
        result.insert(result.end(), list.begin(), list.end());
    }
    return result;
}
