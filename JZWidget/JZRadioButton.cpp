#include "JZRadioButton.h"

#include "JZPainter.h"
#include "JZFontMetrics.h"
#include "JZPen.h"
#include "JZBrush.h"
#include "JZColor.h"

JZ_OBJECT_IMPL(JZRadioButton, JZWidget)

JZRadioButton::JZRadioButton(JZWidget* parent)
    : JZWidget(parent)
    , sigToggled(this)
{}

JZRadioButton::JZRadioButton(JZWidget* parent, const std::string& text)
    : JZRadioButton(parent)
{
    setText(text);
}

void JZRadioButton::setText(const std::string& text)
{
    m_text = text;
}

std::string JZRadioButton::text() const
{ return m_text; }

bool JZRadioButton::isChecked() const
{
    return m_checked;
}

void JZRadioButton::setChecked(bool on)
{
    if (m_checked == on) return;
    m_checked = on;
    update();
}

void JZRadioButton::paintEvent(JZPainterEvent* event)
{
    JZPainter p(this);
    int d = 18;
    int y = (height() - d) / 2;
    int cx = d / 2;
    int cy = y + d / 2;

    // 外环(圆)
    JZPen ringPen(JZColor(0x33, 0x66, 0xcc), 2);
    p.setPen(ringPen);
    p.drawRoundRect(1, y + 1, d - 2, d - 2, (d - 2) / 2);

    // 选中: 内圆点
    if (m_checked) {
        int dd = 8;
        p.fillRect(cx - dd / 2, cy - dd / 2, dd, dd, JZColor(0x33, 0x66, 0xcc));
    }

    // 文本
    JZFont font(14);
    p.setPen(JZPen(JZColor(0x20, 0x20, 0x20), 1));
    p.setFont(font);
    if (!m_text.empty())
        p.drawText(d + 6, JZFontMetrics::centerY(font, height()), m_text.c_str());
}

JZSize JZRadioButton::sizeHint() const
{
    return JZSize(96, 20);
}
