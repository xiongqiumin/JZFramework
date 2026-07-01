#ifndef JZ_LVGL_LED_H_
#define JZ_LVGL_LED_H_

#include "JZLvglWidget.h"

class JZLvglLed : public JZLvglWidget
{
public:
    explicit JZLvglLed(JZLvglWidget* parent = nullptr);

    void setColor(lv_color_t c)  { lv_led_set_color(m_lv, c); }
    void on()                    { lv_led_on(m_lv); }
    void off()                   { lv_led_off(m_lv); }
    void toggle()                { lv_led_toggle(m_lv); }
};

#endif
