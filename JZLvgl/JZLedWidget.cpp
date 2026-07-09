#include "JZLedWidget.h"
#include "lvgl.h"

JZ_OBJECT_IMPL(JZLed, JZWidget)

JZLed::JZLed(JZWidget* parent)
    : JZWidget(lv_led_create, parent)
{
}

void JZLed::setColor(const JZColor& c)
{
    auto pod = c.toLvColor();
    lv_color_t lv;
    lv.red = pod.red; lv.green = pod.green; lv.blue = pod.blue;
    lv_led_set_color(m_lv, lv);
}

void JZLed::on()
{
    lv_led_on(m_lv);
}

void JZLed::off()
{
    lv_led_off(m_lv);
}

void JZLed::toggle()
{
    lv_led_toggle(m_lv);
}
