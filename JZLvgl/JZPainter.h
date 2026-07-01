#ifndef JZ_PAINTER_H_
#define JZ_PAINTER_H_

#include "lvgl.h"
#include "JZPen.h"
#include "JZBrush.h"
#include "JZFont.h"

class JZPainter
{
public:
    explicit JZPainter(lv_event_t* e);

    void setPen(const JZPen& pen)       { m_pen = pen; }
    JZPen pen() const                   { return m_pen; }
    void setBrush(const JZBrush& brush) { m_brush = brush; }
    JZBrush brush() const               { return m_brush; }
    void setFont(const JZFont& font)    { m_font = font; }
    JZFont font() const                 { return m_font; }

    void drawLine(int x1, int y1, int x2, int y2);
    void drawRect(int x, int y, int w, int h);
    void drawRoundRect(int x, int y, int w, int h, int radius);
    void fillRect(int x, int y, int w, int h, lv_color_t color);
    void drawArc(int x, int y, int radius, int startAngle, int endAngle);
    void drawText(int x, int y, const char* text);

    static const lv_font_t* resolveFont(const JZFont& font);

private:
    lv_event_t* m_event;
    lv_layer_t* m_layer = nullptr;
    JZPen m_pen;
    JZBrush m_brush;
    JZFont m_font;
};

#endif
