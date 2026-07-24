#include "JZPen.h"

JZPen::JZPen(const JZColor& color, int width)
    : m_color(color), m_width(width)
{
}

void JZPen::setColor(const JZColor& color)
{
    m_color = color;
}

JZColor JZPen::color() const
{
    return m_color;
}

void JZPen::setWidth(int width)
{
    m_width = width;
}

int JZPen::width() const
{
    return m_width;
}
