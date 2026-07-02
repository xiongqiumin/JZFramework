#include "JZAudioDriver.h"

JZAudioDriver *JZAudioDriver::m_inst = nullptr;

JZAudioDriver::~JZAudioDriver()
{
}

void JZAudioDriver::setInstance(JZAudioDriver *inst)
{
    m_inst = inst;
}

JZAudioDriver *JZAudioDriver::instance()
{
    JZ_ASSERT_X(m_inst, "JZAudioDriver::setInstance first");
    return m_inst;
}