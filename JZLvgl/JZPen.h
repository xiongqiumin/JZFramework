#ifndef JZ_PEN_H_
#define JZ_PEN_H_

#include "lvgl.h"

class JZPen
{
public:
    JZPen() = default;
    JZPen(lv_color_t color, int width = 1) : m_color(color), m_width(width) {}

    void setColor(lv_color_t color) { m_color = color; }
    lv_color_t color() const        { return m_color; }

    void setWidth(int width)        { m_width = width; }
    int width() const               { return m_width; }

private:
    lv_color_t m_color = lv_color_black();
    int m_width = 1;
};

#endif // JZ_PEN_H_
