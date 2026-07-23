

#include "JZPainter.h"
#include "JZWidget.h"
#include <cstring>

JZPainter::JZPainter(JZWidget* widget)
    : m_layer(nullptr)
{}

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
{}

void JZPainter::drawRect(int x, int y, int w, int h)
{}

void JZPainter::drawRoundRect(int x, int y, int w, int h, int radius)
{}

void JZPainter::fillRect(int x, int y, int w, int h, const JZColor& color)
{}

void JZPainter::drawArc(int x, int y, int radius, int startAngle, int endAngle)
{}

void JZPainter::drawText(int x, int y, const char* text)
{}

void JZPainter::drawImage(const JZImage& img, int x, int y, int w, int h)
{}

const void* JZPainter::resolveFont(const JZFont& font)
{
    return nullptr;
}
