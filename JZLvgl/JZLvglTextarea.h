#ifndef JZ_LVGL_TEXTAREA_H_
#define JZ_LVGL_TEXTAREA_H_

#include "JZLvglWidget.h"

class JZLvglTextarea : public JZLvglWidget
{
public:
    explicit JZLvglTextarea(JZLvglWidget* parent = nullptr);

    void setText(const std::string& text);
    std::string text() const;
    void setOneLine(bool one)  { lv_textarea_set_one_line(m_lv, one); }
    void setPassword(bool pw)  { lv_textarea_set_password_mode(m_lv, pw); }
};

#endif
