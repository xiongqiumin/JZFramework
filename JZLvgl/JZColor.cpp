#include "JZColor.h"

JZColor::JZColor()
    : m_r(0), m_g(0), m_b(0)
{
}

JZColor::JZColor(uint8_t r, uint8_t g, uint8_t b)
    : m_r(r), m_g(g), m_b(b)
{
}

uint8_t JZColor::red() const   { return m_r; }
uint8_t JZColor::green() const { return m_g; }
uint8_t JZColor::blue() const  { return m_b; }

void JZColor::setRgb(uint8_t r, uint8_t g, uint8_t b)
{
    m_r = r; m_g = g; m_b = b;
}

lv_color_t JZColor::toLvColor() const
{
    return lv_color_make(m_r, m_g, m_b);
}

JZColor JZColor::fromLvColor(lv_color_t c)
{
    return JZColor(c.ch.red, c.ch.green, c.ch.blue);
}

JZColor JZColor::Black() { return JZColor(0, 0, 0); }
JZColor JZColor::White() { return JZColor(255, 255, 255); }
JZColor JZColor::Red()   { return JZColor(255, 0, 0); }
JZColor JZColor::Green() { return JZColor(0, 255, 0); }
JZColor JZColor::Blue()  { return JZColor(0, 0, 255); }
