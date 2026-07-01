#include "JZLvglSwitch.h"

JZLvglSwitch::JZLvglSwitch(JZLvglWidget* parent)
    : JZLvglWidget(lv_switch_create, parent)
    , sigToggled(this)
{
}

void JZLvglSwitch::onLvEvent(lv_event_t* e)
{
    JZLvglWidget::onLvEvent(e);
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED)
        sigToggled.emit(isOn());
}
