#ifndef JZ_BRUSH_H_
#define JZ_BRUSH_H_

#include "lvgl.h"

class JZBrush
{
public:
    JZBrush() = default;
    JZBrush(lv_color_t color, lv_opa_t opa = LV_OPA_COVER) : m_color(color), m_opa(opa) {}

    void setColor(lv_color_t color) { m_color = color; }
    lv_color_t color() const        { return m_color; }

    void setOpacity(lv_opa_t opa)   { m_opa = opa; }
    lv_opa_t opacity() const        { return m_opa; }

private:
    lv_color_t m_color = lv_color_black();
    lv_opa_t m_opa = LV_OPA_COVER;
};

#endif // JZ_BRUSH_H_
