#include "JZLvglDropdown.h"

JZLvglDropdown::JZLvglDropdown(JZLvglWidget* parent)
    : JZLvglWidget(lv_dropdown_create, parent)
    , sigValueChanged(this)
{
}

void JZLvglDropdown::onLvEvent(lv_event_t* e)
{
    JZLvglWidget::onLvEvent(e);
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED)
        sigValueChanged.emit((int)selected());
}
