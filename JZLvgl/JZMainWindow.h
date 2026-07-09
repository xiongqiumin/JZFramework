#ifndef JZ_LVGL_WINDOW_H_
#define JZ_LVGL_WINDOW_H_

#include "JZWidget.h"

class JZMainWindow : public JZWidget
{
    JZ_OBJECT_DECLARE(JZMainWindow)

public:
    explicit JZMainWindow(JZWidget* parent = nullptr);
    JZMainWindow(JZWidget* parent, int w, int h, const std::string& title);

    std::string title() const;
    void setTitle(const std::string& title);
    lv_obj_t* addButton(const void* icon, int32_t btn_w);

private:
    std::string m_title;
};

#endif
