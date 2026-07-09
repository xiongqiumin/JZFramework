#include "JZKeyEvent.h"
#include "lvgl.h"

JZKeyEvent::JZKeyEvent(lv_event_t* e)
    : m_event(e)
{
}

uint32_t JZKeyEvent::key() const
{
    return lv_indev_get_key(lv_indev_active());
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
