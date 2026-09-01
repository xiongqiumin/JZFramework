#include "JZKeyEvent.h"

JZKeyEvent::JZKeyEvent(uint32_t key, uint32_t modifiers, const std::string& text, bool repeat)
    : JZEvent(JZEvent_key)
    , m_key(key)
    , m_modifiers(modifiers)
    , m_text(text)
    , m_repeat(repeat)
{
}

uint32_t JZKeyEvent::key() const
{
    return m_key;
}

uint32_t JZKeyEvent::modifiers() const
{
    return m_modifiers;
}

const std::string& JZKeyEvent::text() const
{
    return m_text;
}

bool JZKeyEvent::isRepeat() const
{
    return m_repeat;
}

bool JZKeyEvent::isPress() const
{
    return m_press;
}

void JZKeyEvent::setPress(bool press)
{
    m_press = press;
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
