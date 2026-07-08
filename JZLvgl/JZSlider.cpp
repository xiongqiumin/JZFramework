#include "JZSlider.h"

JZ_OBJECT_IMPL(JZSlider, JZWidget)

JZSlider::JZSlider(JZWidget* parent)
    : JZWidget(lv_slider_create, parent)
    , sigValueChanged(this)
{
}

void JZSlider::setRange(int32_t min, int32_t max)
{
    lv_slider_set_range(m_lv, min, max);
}

void JZSlider::setValue(int32_t v)
{
    lv_slider_set_value(m_lv, v, LV_ANIM_OFF);
}

int32_t JZSlider::value() const
{
    return lv_slider_get_value(m_lv);
}

void JZSlider::onLvEvent(lv_event_t* e)
{
    JZWidget::onLvEvent(e);
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED)
        sigValueChanged.emit(value());
}
