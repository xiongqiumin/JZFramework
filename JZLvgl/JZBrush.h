#ifndef JZ_BRUSH_H_
#define JZ_BRUSH_H_

#include "lvgl.h"
#include "JZColor.h"

class JZBrush
{
public:
    JZBrush() = default;
    JZBrush(const JZColor& color, lv_opa_t opa = LV_OPA_COVER);

    void setColor(const JZColor& color);
    JZColor color() const;

    void setOpacity(lv_opa_t opa);
    lv_opa_t opacity() const;

private:
    JZColor m_color = JZColor::Black();
    lv_opa_t m_opa = LV_OPA_COVER;
};

#endif // JZ_BRUSH_H_
