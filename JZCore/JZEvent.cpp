#include "JZEvent.h"

JZEvent::JZEvent(int type)
{
    m_type = type;
}

JZEvent::~JZEvent()
{
}

int JZEvent::type() const
{
    return m_type;
}

//JZTimerEvent
JZTimerEvent::JZTimerEvent(int id)
    :JZEvent(JZEvent_timer)
{
    m_id = id;
}

JZTimerEvent::~JZTimerEvent()
{
}


int JZTimerEvent::id() const
{
    return m_id;
}
