#include "JZLabel.h"
#include "JZPainter.h"
#include "JZFontMetrics.h"

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
    // 按文本内容给期望尺寸 (空文本给最小占位)
    if (m_text.empty())
        return JZSize(80, 20);
    JZFont font(14);
    return JZSize(JZFontMetrics::textWidth(m_text, font) + 8,
                  JZFontMetrics::textHeight(font) + 4);
}

void JZLabel::paintEvent(JZPainterEvent* event)
{
    JZPainter p(this);
    int w = width(), h = height();

    p.setPen(JZPen(JZColor(0x33, 0x33, 0x33), 1));
    JZFont font(14);
    p.setFont(font);

    if (m_text.empty())
        return;

    int tw = JZFontMetrics::textWidth(m_text, font);
    int tx = (m_align == 1) ? (w - tw) / 2 : (m_align == 2) ? (w - tw - 4) : 4;
    p.drawText(tx, JZFontMetrics::centerY(font, h), m_text.c_str());
}
