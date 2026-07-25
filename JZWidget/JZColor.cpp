#include "JZColor.h"

JZColor::JZColor()
    : m_r(0), m_g(0), m_b(0), m_a(255)
{
}

JZColor::JZColor(uint8_t r, uint8_t g, uint8_t b,uint8_t a)
    : m_r(r), m_g(g), m_b(b), m_a(a)
{
}

JZColor::JZColor(ColorEnum color)
{
    switch (color) {
    case Black:      setRgb(0, 0, 0);       break;
    case White:      setRgb(255, 255, 255); break;
    case Red:        setRgb(255, 0, 0);     break;
    case Green:      setRgb(0, 255, 0);     break;
    case Blue:       setRgb(0, 0, 255);     break;
    case Yellow:     setRgb(255, 255, 0);   break;
    case Cyan:       setRgb(0, 255, 255);   break;
    case Magenta:    setRgb(255, 0, 255);   break;
    case Gray:       setRgb(128, 128, 128); break;
    case LightGray:  setRgb(211, 211, 211); break;
    case DarkGray:   setRgb(64, 64, 64);    break;
    case Orange:     setRgb(255, 165, 0);   break;
    case Purple:     setRgb(128, 0, 128);   break;
    case Transparent:setRgba(0, 0, 0, 0);   break; // 无 alpha 通道, 退化为黑
    }
}

uint8_t JZColor::red() const   { return m_r; }
uint8_t JZColor::green() const { return m_g; }
uint8_t JZColor::blue() const  { return m_b; }

void JZColor::setRgb(uint8_t r, uint8_t g, uint8_t b)
{
    m_r = r; m_g = g; m_b = b; m_a = 255;
}

void JZColor::setRgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    m_r = r; m_g = g; m_b = b; m_a = a;
}
