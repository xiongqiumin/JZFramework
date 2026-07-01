#include "JZLvglSlider.h"

JZLvglSlider::JZLvglSlider(JZLvglWidget* parent)
    : JZLvglWidget(lv_slider_create, parent)
    , sigValueChanged(this)
{
}

void JZLvglSlider::onLvEvent(lv_event_t* e)
{
    JZLvglWidget::onLvEvent(e);
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED)
        sigValueChanged.emit(value());
}
