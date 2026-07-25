#include "JZOTADriver.h"
#include "JZDebug.h"

JZOTADriver *JZOTADriver::m_inst = nullptr;

void JZOTADriver::setInstance(JZOTADriver *inst)
{
    if (m_inst && m_inst != inst)
    {
        if (inst)
            jzWarning("JZOTADriver instance already set, replacing old driver");
        delete m_inst;
    }
    m_inst = inst;
}

JZOTADriver *JZOTADriver::instance()
{
    JZ_ASSERT_X(m_inst, "JZOTADriver::setInstance first");
    return m_inst;
}
