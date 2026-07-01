#include "JZLvglArc.h"

JZLvglArc::JZLvglArc(JZLvglWidget* parent)
    : JZLvglWidget(lv_arc_create, parent)
    , sigValueChanged(this)
{
}

void JZLvglArc::onLvEvent(lv_event_t* e)
{
    JZLvglWidget::onLvEvent(e);
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED)
        sigValueChanged.emit(value());
}
