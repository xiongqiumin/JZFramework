#include "JZPainter.h"
#include "JZWidget.h"
#include "lvgl.h"
#include <cstring>

// JZColor::lv_color_pod → lv_color_t
static inline lv_color_t toLvColor(const JZColor::lv_color_pod& c) {
    lv_color_t out;
    out.red = c.red; out.green = c.green; out.blue = c.blue;
    return out;
}

JZPainter::JZPainter(JZWidget* widget)
    : m_layer(nullptr)
{
    m_layer = widget->paintLayer();
    JZ_ASSERT(m_layer);
}

void JZPainter::setPen(const JZPen& pen)
{
    m_pen = pen;
}

JZPen JZPainter::pen() const
{
    return m_pen;
}

void JZPainter::setBrush(const JZBrush& brush)
{
    m_brush = brush;
}

JZBrush JZPainter::brush() const
{
    return m_brush;
}

void JZPainter::setFont(const JZFont& font)
{
    m_font = font;
}

JZFont JZPainter::font() const
{
    return m_font;
}

void JZPainter::drawLine(int x1, int y1, int x2, int y2)
{
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = toLvColor(m_pen.color().toLvColor());
    dsc.width = m_pen.width();
    dsc.p1.x = x1; dsc.p1.y = y1;
    dsc.p2.x = x2; dsc.p2.y = y2;
    lv_draw_line(m_layer, &dsc);
}

void JZPainter::drawRect(int x, int y, int w, int h)
{
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_opa = LV_OPA_TRANSP;
    dsc.border_color = toLvColor(m_pen.color().toLvColor());
    dsc.border_width = m_pen.width();
    lv_area_t area = {(lv_coord_t)x, (lv_coord_t)y, (lv_coord_t)(x + w - 1), (lv_coord_t)(y + h - 1)};
    lv_draw_rect(m_layer, &dsc, &area);
}

void JZPainter::drawRoundRect(int x, int y, int w, int h, int radius)
{
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_opa = LV_OPA_TRANSP;
    dsc.border_color = toLvColor(m_pen.color().toLvColor());
    dsc.border_width = m_pen.width();
    dsc.radius = (lv_coord_t)radius;
    lv_area_t area = {(lv_coord_t)x, (lv_coord_t)y, (lv_coord_t)(x + w - 1), (lv_coord_t)(y + h - 1)};
    lv_draw_rect(m_layer, &dsc, &area);
}

void JZPainter::fillRect(int x, int y, int w, int h, const JZColor& color)
{
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = toLvColor(color.toLvColor());
    dsc.bg_opa = LV_OPA_COVER;
    lv_area_t area = {(lv_coord_t)x, (lv_coord_t)y, (lv_coord_t)(x + w - 1), (lv_coord_t)(y + h - 1)};
    lv_draw_rect(m_layer, &dsc, &area);
}

void JZPainter::drawArc(int x, int y, int radius, int startAngle, int endAngle)
{
    lv_draw_arc_dsc_t dsc;
    lv_draw_arc_dsc_init(&dsc);
    dsc.color = toLvColor(m_pen.color().toLvColor());
    dsc.width = m_pen.width();
    dsc.center.x = (lv_coord_t)x;
    dsc.center.y = (lv_coord_t)y;
    dsc.radius = (uint16_t)radius;
    dsc.start_angle = startAngle;
    dsc.end_angle = endAngle;
    lv_draw_arc(m_layer, &dsc);
}

void JZPainter::drawText(int x, int y, const char* text)
{
    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.color = toLvColor(m_brush.color().toLvColor());
    dsc.font = resolveFont(m_font);
    lv_area_t area = {(lv_coord_t)x, (lv_coord_t)y, (lv_coord_t)(x + 1000), (lv_coord_t)(y + 100)};
    lv_draw_label(m_layer, &dsc, &area);
}

const lv_font_t* JZPainter::resolveFont(const JZFont& font)
{
    return LV_FONT_DEFAULT;
}
