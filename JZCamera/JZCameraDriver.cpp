#include "JZCameraDriver.h"

JZCameraDriver *JZCameraDriver::s_instance = nullptr;

JZCameraDriver *JZCameraDriver::instance()
{
    return s_instance;
}

void JZCameraDriver::setInstance(JZCameraDriver *inst)
{
    s_instance = inst;
}

void JZCameraDriver::clearInstance()
{
    s_instance = nullptr;
}
