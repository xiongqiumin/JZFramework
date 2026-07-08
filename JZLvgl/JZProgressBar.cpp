#include "JZProgressBar.h"

JZ_OBJECT_IMPL(JZProgressBar, JZWidget)

JZProgressBar::JZProgressBar(JZWidget* parent)
    : JZWidget(lv_bar_create, parent)
{
}

void JZProgressBar::setRange(int32_t min, int32_t max)
{
    lv_bar_set_range(m_lv, min, max);
}

void JZProgressBar::setValue(int32_t v, bool anim)
{
    lv_bar_set_value(m_lv, v, anim ? LV_ANIM_ON : LV_ANIM_OFF);
}

int32_t JZProgressBar::value() const
{
    return lv_bar_get_value(m_lv);
}
