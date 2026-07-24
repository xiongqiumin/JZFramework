#include "JZOTADriver.h"

JZOTADriver *JZOTADriver::m_inst = nullptr;

void JZOTADriver::setInstance(JZOTADriver *inst)
{
    m_inst = inst;
}

JZOTADriver *JZOTADriver::instance()
{
    JZ_ASSERT_X(m_inst, "JZOTADriver::setInstance first");
    return m_inst;
}
