#include "JZSlider.h"
#include "JZPainter.h"
#include <algorithm>

JZ_OBJECT_IMPL(JZSlider, JZWidget)

JZSlider::JZSlider(JZWidget* parent)
    : JZWidget(parent)
    , sigValueChanged(this)
{
}

void JZSlider::setRange(int32_t min, int32_t max)
{
    m_min = min;
    m_max = max;
}

void JZSlider::setValue(int32_t v)
{
    if (m_max > m_min)
        m_value = std::max(m_min, std::min(v, m_max));
    else
        m_value = v;
}

int32_t JZSlider::value() const
{ return m_value; }

JZSize JZSlider::sizeHint() const
{
    int w = m_width > 0 ? m_width : 120;
    return JZSize(w, 24);
}

void JZSlider::paintEvent(JZPainterEvent* event)
{
    JZPainter p(this);
    int w = width(), h = height();

    int trackH = 4;
    int ty = (h - trackH) / 2;
    p.setBrush(JZBrush(JZColor(0xcc, 0xcc, 0xcc)));
    p.setPen(JZPen(JZColor(0xbb, 0xbb, 0xbb), 1));
    p.drawRoundRect(2, ty, w - 4, trackH, trackH / 2);

    int range = m_max - m_min;
    int pos = (range > 0) ? (w - 12) * (m_value - m_min) / range + 6 : w / 2;
    int knob = 12;
    p.setBrush(JZBrush(JZColor(0x33, 0x66, 0xcc)));
    p.setPen(JZPen(JZColor(0x33, 0x66, 0xcc), 1));
    p.drawRoundRect(pos - knob / 2, (h - knob) / 2, knob, knob, knob / 2);
}
