#include "JZKeyEvent.h"

JZKeyEvent::JZKeyEvent()
    : JZEvent(JZEvent_key)
{
}

uint32_t JZKeyEvent::key() const
{
    return 0;
}

bool JZKeyEvent::isAccepted() const
{
    return m_accepted;
}

void JZKeyEvent::accept()
{
    m_accepted = true;
}

void JZKeyEvent::ignore()
{
    m_accepted = false;
}
