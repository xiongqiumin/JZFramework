#ifndef JZ_LVGL_LED_H_
#define JZ_LVGL_LED_H_

#include "JZWidget.h"
#include "JZColor.h"

class JZLed : public JZWidget
{
    JZ_OBJECT_DECLARE(JZLed)

public:
    explicit JZLed(JZWidget* parent = nullptr);

    void setColor(const JZColor& c);
    void on();
    void off();
    void toggle();
};

#endif
