#include "JZMouseEvent.h"

JZMouseEvent::JZMouseEvent(MouseType type, Button button, const JZPoint& globalPos,
                           const JZPoint& localPos, int wheelDelta, uint32_t modifiers)
    : JZEvent(JZEvent_mouse)
    , m_mouseType(type)
    , m_button(button)
    , m_globalPos(globalPos)
    , m_localPos(localPos)
    , m_wheelDelta(wheelDelta)
    , m_modifiers(modifiers)
{
}
