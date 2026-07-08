#include "JZArcWidget.h"

JZ_OBJECT_IMPL(JZArcWidget, JZWidget)

JZArcWidget::JZArcWidget(JZWidget* parent)
    : JZWidget(lv_arc_create, parent)
    , sigValueChanged(this)
{
}

void JZArcWidget::setRange(int32_t min, int32_t max)
{
    lv_arc_set_range(m_lv, min, max);
}

void JZArcWidget::setValue(int32_t v)
{
    lv_arc_set_value(m_lv, v);
}

int32_t JZArcWidget::value() const
{
    return lv_arc_get_value(m_lv);
}

void JZArcWidget::setAngles(uint16_t bgStart, uint16_t bgEnd)
{
    lv_arc_set_bg_angles(m_lv, bgStart, bgEnd);
}

void JZArcWidget::setRotation(uint16_t rot)
{
    lv_arc_set_rotation(m_lv, rot);
}

void JZArcWidget::onLvEvent(lv_event_t* e)
{
    JZWidget::onLvEvent(e);
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED)
        sigValueChanged.emit(value());
}
