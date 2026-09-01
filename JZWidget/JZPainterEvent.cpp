#include "JZPainterEvent.h"

JZInvalidateEvent::JZInvalidateEvent()
    :JZEvent(JZEvent_invalidate)
{
}

JZPainterEvent::JZPainterEvent()
    :JZEvent(JZEvent_paint)
{
}