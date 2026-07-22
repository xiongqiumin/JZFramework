#ifndef JZ_LVGL_LED_H_
#define JZ_LVGL_LED_H_

#include "JZWidget.h"
#include "JZColor.h"

class JZLedWidget : public JZWidget
{
    JZ_OBJECT_DECLARE(JZLedWidget)

public:
    explicit JZLedWidget(JZWidget* parent = nullptr);

    void setColor(const JZColor& c);
    void on();
    void off();
    void toggle();
};

#endif
