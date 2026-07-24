#include "JZPainter.h"
#include "JZWidget.h"
#include <cstring>
#include <algorithm>
#include <cstdlib>

JZPainter::JZPainter(JZWidget* widget)
{
    m_layer = nullptr;
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
    if (!m_layer) return;
    JZDrawTask t;
    t.type = JZDrawTaskType::Rect;
    t.clip = m_clip;
    t.color = toPixel(m_pen.color());
    if (x1 == x2) {
        int y = std::min(y1, y2), h = std::abs(y2 - y1) + 1;
        t.area = JZRect(m_ox + x1, m_oy + y, 1, h);
    } else if (y1 == y2) {
        int x = std::min(x1, x2), w = std::abs(x2 - x1) + 1;
        t.area = JZRect(m_ox + x, m_oy + y1, w, 1);
    } else {
        // 斜线暂按包围盒填充 (TODO: 实现 Bresenham)
        int x = std::min(x1, x2), y = std::min(y1, y2);
        int w = std::abs(x2 - x1) + 1, h = std::abs(y2 - y1) + 1;
        t.area = JZRect(m_ox + x, m_oy + y, w, h);
    }
    m_layer->addTask(t);
}

void JZPainter::drawRect(int x, int y, int w, int h)
{
    if (!m_layer) return;
    JZDrawTask t;
    t.type = JZDrawTaskType::Border;
    t.area = JZRect(m_ox + x, m_oy + y, w, h);
    t.clip = m_clip;
    t.color = toPixel(m_pen.color());
    t.borderWidth = m_pen.width() > 0 ? m_pen.width() : 1;
    m_layer->addTask(t);
}

void JZPainter::drawRoundRect(int x, int y, int w, int h, int radius)
{
    if (!m_layer) return;
    JZDrawTask t;
    t.type = JZDrawTaskType::Rect;
    t.area = JZRect(m_ox + x, m_oy + y, w, h);
    t.clip = m_clip;
    t.color = toPixel(m_brush.color());
    t.radius = radius; // TODO: JZLayer 暂按直角填充
    m_layer->addTask(t);
}

void JZPainter::fillRect(int x, int y, int w, int h, const JZColor& color)
{
    if (!m_layer) return;
    JZDrawTask t;
    t.type = JZDrawTaskType::Rect;
    t.area = JZRect(m_ox + x, m_oy + y, w, h);
    t.clip = m_clip;
    t.color = toPixel(color);
    m_layer->addTask(t);
}

void JZPainter::drawArc(int x, int y, int radius, int startAngle, int endAngle)
{
    // TODO: 适配 JZDrawTask (扇形/弧线), 暂留空
}

void JZPainter::drawText(int x, int y, const char* text)
{
    // TODO: 文本光栅化需要字体引擎, 暂留空 (参考 lv_draw_label / draw/sw)
}

void JZPainter::drawImage(const JZImage& img, int x, int y, int w, int h)
{
    // TODO: 图像混合 (参考 lv_draw_image / draw/sw)
}

const void* JZPainter::resolveFont(const JZFont& font)
{
    return nullptr;
}
