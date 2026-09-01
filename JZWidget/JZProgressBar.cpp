#include "JZProgressBar.h"
#include "JZPainter.h"
#include <algorithm>
#include <cstdio>

JZ_OBJECT_IMPL(JZProgressBar, JZWidget)

JZProgressBar::JZProgressBar(JZWidget* parent)
    : JZWidget(parent)
{
}

void JZProgressBar::setRange(int32_t min, int32_t max)
{
    m_min = min;
    m_max = max;
}

void JZProgressBar::setValue(int32_t v, bool anim)
{
    if (m_max > m_min)
        m_value = std::max(m_min, std::min(v, m_max));
    else
        m_value = v;
}

int32_t JZProgressBar::value() const
{ return m_value; }

JZSize JZProgressBar::sizeHint() const
{
    int w = m_width > 0 ? m_width : 120;
    return JZSize(w, 16);
}

void JZProgressBar::paintEvent(JZPainterEvent* event)
{
    JZPainter p(this);
    int w = width(), h = height();

    p.setBrush(JZBrush(JZColor(0xdd, 0xdd, 0xdd)));
    p.setPen(JZPen(JZColor(0xcc, 0xcc, 0xcc), 1));
    p.drawRoundRect(0, 0, w, h, h / 2);

    int range = m_max - m_min;
    int fillW = (range > 0) ? w * (m_value - m_min) / range : 0;
    if (fillW > 0) {
        fillW = std::max(2, std::min(fillW, w));
        p.setBrush(JZBrush(JZColor(0x33, 0x99, 0x33)));
        p.drawRoundRect(0, 0, fillW, h, h / 2);
    }

    // 居中百分比文本
    int pct = (range > 0) ? (100 * (m_value - m_min)) / range : 0;
    char buf[16];
    snprintf(buf, sizeof(buf), "%d%%", pct);
    std::string txt(buf);
    p.setPen(JZPen(JZColor(0x33, 0x33, 0x33), 1));
    p.setFont(JZFont(11));
    p.drawText(w / 2 - (int)txt.size() * 3, (h - 12) / 2, txt.c_str());
}
