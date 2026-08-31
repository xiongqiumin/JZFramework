#include "JZKeyEvent.h"

JZKeyEvent::JZKeyEvent(uint32_t key)
    : JZEvent(JZEvent_key)
    , m_key(key)
{
}

uint32_t JZKeyEvent::key() const
{
    return m_key;
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
