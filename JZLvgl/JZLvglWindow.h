#ifndef JZ_LVGL_WINDOW_H_
#define JZ_LVGL_WINDOW_H_

#include "JZLvglWidget.h"

class JZLvglWindow : public JZLvglWidget
{
public:
    explicit JZLvglWindow(JZLvglWidget* parent = nullptr);
    JZLvglWindow(JZLvglWidget* parent, int w, int h, const std::string& title);

    lv_obj_t* contentLv() const;
    lv_obj_t* headerLv() const;

    void addTitleBtn(const void* icon, int32_t btnW);
};

#endif
