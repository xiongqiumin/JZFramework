#ifndef JZ_LVGL_WINDOW_H_
#define JZ_LVGL_WINDOW_H_

#include "JZWidget.h"

class JZMainWindow : public JZWidget
{
    JZ_OBJECT_DECLARE(JZMainWindow)

public:
    explicit JZMainWindow(JZWidget* parent = nullptr);
    JZMainWindow(JZWidget* parent, int w, int h, const std::string& title);

    void addTitleBtn(const void* icon, int32_t btnW);
};

#endif
