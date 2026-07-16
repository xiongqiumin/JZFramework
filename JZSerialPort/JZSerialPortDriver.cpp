#include "JZSerialPortDriver.h"

JZSerialPortDriver *JZSerialPortDriver::s_instance = nullptr;

JZSerialPortDriver *JZSerialPortDriver::instance()
{
    JZ_ASSERT_X(s_instance, "JZSerialPortDriver not init");
    return s_instance;
}

void JZSerialPortDriver::setInstance(JZSerialPortDriver *inst)
{
    if(s_instance)
    {
        delete s_instance;
    }
    s_instance = inst;
}
