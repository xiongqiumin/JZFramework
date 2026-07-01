#ifndef JZ_LVGL_LABEL_H_
#define JZ_LVGL_LABEL_H_

#include "JZLvglWidget.h"

class JZLvglLabel : public JZLvglWidget
{
public:
    explicit JZLvglLabel(JZLvglWidget* parent = nullptr);
    JZLvglLabel(JZLvglWidget* parent, const std::string& text);

    void setText(const std::string& text);
    std::string text() const;

    void setAlign(lv_text_align_t align)  { lv_obj_set_style_text_align(m_lv, align, 0); }
    void setLongMode(lv_label_long_mode_t mode) { lv_label_set_long_mode(m_lv, mode); }
};

#endif
