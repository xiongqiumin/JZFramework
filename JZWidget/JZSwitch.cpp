#include "JZSwitch.h"

JZ_OBJECT_IMPL(JZSwitch, JZWidget)

JZSwitch::JZSwitch(JZWidget* parent)
    : JZWidget(lv_switch_create, parent)
    , sigToggled(this)
{
}

void JZSwitch::setState(bool on)
{
    if (on)
        lv_obj_add_state(m_lv, LV_STATE_CHECKED);
    else
        lv_obj_remove_state(m_lv, LV_STATE_CHECKED);
}

bool JZSwitch::isOn() const
{
    return lv_obj_has_state(m_lv, LV_STATE_CHECKED);
}

void JZSwitch::onLvEvent(lv_event_t* e)
{
    JZWidget::onLvEvent(e);
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED)
        sigToggled.emit(isOn());
}
