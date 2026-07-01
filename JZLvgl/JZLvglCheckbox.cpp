#include "JZLvglCheckbox.h"

JZLvglCheckbox::JZLvglCheckbox(JZLvglWidget* parent)
    : JZLvglWidget(lv_checkbox_create, parent)
    , sigToggled(this)
{
}

void JZLvglCheckbox::setText(const std::string& text)
{
    lv_checkbox_set_text(m_lv, text.c_str());
}

std::string JZLvglCheckbox::text() const
{
    return lv_checkbox_get_text(m_lv);
}

bool JZLvglCheckbox::isChecked() const
{
    return lv_obj_has_state(m_lv, LV_STATE_CHECKED);
}
