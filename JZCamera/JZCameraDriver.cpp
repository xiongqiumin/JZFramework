#include "JZCameraDriver.h"
#include "JZDebug.h"

JZCameraDriver *JZCameraDriver::s_instance = nullptr;

JZCameraDriver *JZCameraDriver::instance()
{
    return s_instance;
}

void JZCameraDriver::setInstance(JZCameraDriver *inst)
{
    if (s_instance && s_instance != inst)
    {
        if (inst)
            jzWarning("JZCameraDriver instance already set, replacing old driver");
        delete s_instance;
    }
    s_instance = inst;
}

void JZCameraDriver::clearInstance()
{
    s_instance = nullptr;
}
