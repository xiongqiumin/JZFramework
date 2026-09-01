#include "JZCheckbox.h"
#include "JZPainter.h"
#include "JZFontMetrics.h"

JZ_OBJECT_IMPL(JZCheckbox, JZWidget)

JZCheckbox::JZCheckbox(JZWidget* parent)
    : JZWidget(parent)
    , sigToggled(this)
{
}

void JZCheckbox::setText(const std::string& text)
{
    m_text = text;
}

std::string JZCheckbox::text() const
{
    return m_text;
}

bool JZCheckbox::isChecked() const
{
    return m_checked;
}

void JZCheckbox::setChecked(bool on)
{
    m_checked = on;
}

JZSize JZCheckbox::sizeHint() const
{
    return JZSize(96, 24);
}

void JZCheckbox::paintEvent(JZPainterEvent* event)
{
    JZPainter p(this);
    int h = height();
    int d = 16;
    int cy = (h - d) / 2;

    p.setBrush(JZBrush(m_checked ? JZColor(0x33, 0x66, 0xcc) : JZColor(0xff, 0xff, 0xff)));
    p.setPen(JZPen(JZColor(0x80, 0x80, 0x80), 1));
    p.drawRoundRect(0, cy, d, d, 3);

    if (m_checked) {
        p.setPen(JZPen(JZColor(0xff, 0xff, 0xff), 2));
        p.drawLine(4, cy + d / 2, 7, cy + d - 4);
        p.drawLine(7, cy + d - 4, 12, cy + 3);
    }

    p.setPen(JZPen(JZColor(0x20, 0x20, 0x20), 1));
    JZFont font(14);
    p.setFont(font);
    if (!m_text.empty())
        p.drawText(d + 6, JZFontMetrics::centerY(font, h), m_text.c_str());
}
