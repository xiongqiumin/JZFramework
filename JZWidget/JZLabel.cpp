#include "JZLabel.h"
#include "JZPainter.h"

JZ_OBJECT_IMPL(JZLabel, JZWidget)

JZLabel::JZLabel(JZWidget* parent)
    : JZWidget(parent)
{
}

JZLabel::JZLabel(JZWidget* parent, const std::string& text)
    : JZLabel(parent)
{
    setText(text);
}

void JZLabel::setText(const std::string& text)
{
    m_text = text;
}

std::string JZLabel::text() const
{ 
    return m_text; 
}

void JZLabel::setAlign(int align)
{
    m_align = align;
}

JZSize JZLabel::sizeHint() const
{
    return JZSize(80, 20);
}

void JZLabel::paintEvent(JZPainterEvent* event)
{
    JZPainter p(this);
    int w = width(), h = height();

    p.setPen(JZPen(JZColor(0x33, 0x33, 0x33), 1));
    p.setFont(JZFont(14));

    if (m_text.empty())
        return;

    int tw = (int)m_text.size() * 7;   // 估算文本宽
    int tx = (m_align == 1) ? (w - tw) / 2 : (m_align == 2) ? (w - tw - 4) : 4;
    int ty = (h - 14) / 2;
    p.drawText(tx, ty, m_text.c_str());
}
