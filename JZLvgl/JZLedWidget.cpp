#include "JZLed.h"

JZ_OBJECT_IMPL(JZLed, JZWidget)

JZLed::JZLed(JZWidget* parent)
    : JZWidget(lv_led_create, parent)
{
}

void JZLed::setColor(const JZColor& c)
{
    lv_led_set_color(m_lv, c.toLvColor());
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
