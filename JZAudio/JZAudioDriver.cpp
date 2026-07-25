#include "JZAudioDriver.h"
#include "JZDebug.h"

JZAudioDriver *JZAudioDriver::m_inst = nullptr;

JZAudioDriver::~JZAudioDriver()
{
}

void JZAudioDriver::setInstance(JZAudioDriver *inst)
{
    if (m_inst && m_inst != inst)
    {
        if (inst)
            jzWarning("JZAudioDriver instance already set, replacing old driver");
        delete m_inst;
    }
    m_inst = inst;
}

JZAudioDriver *JZAudioDriver::instance()
{
    JZ_ASSERT_X(m_inst, "JZAudioDriver::setInstance first");
    return m_inst;
}