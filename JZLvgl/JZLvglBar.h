#ifndef JZ_LVGL_BAR_H_
#define JZ_LVGL_BAR_H_

#include "JZLvglWidget.h"

class JZLvglBar : public JZLvglWidget
{
public:
    explicit JZLvglBar(JZLvglWidget* parent = nullptr);

    void setRange(int32_t min, int32_t max) { lv_bar_set_range(m_lv, min, max); }
    void setValue(int32_t v, bool anim = false) { lv_bar_set_value(m_lv, v, anim ? LV_ANIM_ON : LV_ANIM_OFF); }
    int32_t value() const                    { return lv_bar_get_value(m_lv); }
};

#endif
