#include "JZRoller.h"

JZ_OBJECT_IMPL(JZRoller, JZWidget)

JZRoller::JZRoller(JZWidget* parent)
    : JZWidget(lv_roller_create, parent)
{
}

void JZRoller::setOptions(const std::string& opts, lv_roller_mode_t mode)
{
    lv_roller_set_options(m_lv, opts.c_str(), mode);
}

void JZRoller::setVisibleRowCount(uint8_t n)
{
    lv_roller_set_visible_row_count(m_lv, n);
}

void JZRoller::setSelected(uint32_t idx, bool anim)
{
    lv_roller_set_selected(m_lv, idx, anim ? LV_ANIM_ON : LV_ANIM_OFF);
}

uint32_t JZRoller::selected() const
{
    return lv_roller_get_selected(m_lv);
}
