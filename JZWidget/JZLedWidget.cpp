#include "JZLedWidget.h"
#include "lvgl.h"

JZ_OBJECT_IMPL(JZLedWidget, JZWidget)

JZLedWidget::JZLedWidget(JZWidget* parent)
    : JZWidget(lv_led_create, parent)
{
}

void JZLedWidget::setColor(const JZColor& c)
{
    auto pod = c.toLvColor();
    lv_color_t lv;
    lv.red = pod.red; lv.green = pod.green; lv.blue = pod.blue;
    lv_led_set_color(m_lv, lv);
}

void JZLedWidget::on()
{
    lv_led_on(m_lv);
}

void JZLedWidget::off()
{
    lv_led_off(m_lv);
}

void JZLedWidget::toggle()
{
    lv_led_toggle(m_lv);
}
