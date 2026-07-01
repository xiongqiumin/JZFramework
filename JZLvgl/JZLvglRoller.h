#ifndef JZ_LVGL_ROLLER_H_
#define JZ_LVGL_ROLLER_H_

#include "JZLvglWidget.h"

class JZLvglRoller : public JZLvglWidget
{
public:
    explicit JZLvglRoller(JZLvglWidget* parent = nullptr);

    void setOptions(const std::string& opts, lv_roller_mode_t mode = LV_ROLLER_MODE_NORMAL) { lv_roller_set_options(m_lv, opts.c_str(), mode); }
    void setVisibleRowCount(uint8_t n)  { lv_roller_set_visible_row_count(m_lv, n); }
    void setSelected(uint32_t idx, bool anim = false) { lv_roller_set_selected(m_lv, idx, anim ? LV_ANIM_ON : LV_ANIM_OFF); }
    uint32_t selected() const           { return lv_roller_get_selected(m_lv); }
};

#endif
