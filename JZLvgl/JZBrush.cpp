#include "JZBrush.h"

JZBrush::JZBrush(const JZColor& color, lv_opa_t opa)
    : m_color(color), m_opa(opa)
{
}

void JZBrush::setColor(const JZColor& color)
{
    m_color = color;
}

JZColor JZBrush::color() const
{
    return m_color;
}

void JZBrush::setOpacity(lv_opa_t opa)
{
    m_opa = opa;
}

lv_opa_t JZBrush::opacity() const
{
    return m_opa;
}
