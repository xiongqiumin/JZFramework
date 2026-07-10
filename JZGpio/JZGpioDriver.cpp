#include "JZGpioDriver.h"

JZGpioDriver *JZGpioDriver::s_instance = nullptr;

JZGpioDriver *JZGpioDriver::instance()
{
    JZ_ASSERT_X(s_instance, "JZGpioDriver not init");
    return s_instance;
}

void JZGpioDriver::setInstance(JZGpioDriver *inst)
{
    s_instance = inst;
}

void JZGpioDriver::clearInstance()
{
    s_instance = nullptr;
}
