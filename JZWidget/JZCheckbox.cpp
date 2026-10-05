#include "JZCheckbox.h"
#include "JZPainter.h"
#include "JZFontMetrics.h"

JZ_OBJECT_IMPL(JZCheckbox, JZWidget)

JZCheckbox::JZCheckbox(JZWidget* parent)
    : JZWidget(parent)
    , sigToggled(this)
{}

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
    if (m_checked == on)
        return;
    m_checked = on;
    update();
    notifyButtonGroupChecked();
}

JZSize JZCheckbox::sizeHint() const
{
    return JZSize(96, 24);
}

JZSize JZCheckbox::minimumSizeHint() const
{
    return JZSize(0, 22);
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

// ==================== 鼠标交互 ====================
// 左键按下 + 本控件内释放 -> 翻转选中态
void JZCheckbox::mousePressEvent(JZMouseEvent* event)
{
    if (event->button() == JZMouseEvent::LeftButton)
        m_pressed = true;
}

void JZCheckbox::mouseReleaseEvent(JZMouseEvent* event)
{
    if (event->button() != JZMouseEvent::LeftButton) {
        JZWidget::mouseReleaseEvent(event);
        return;
    }
    bool wasPressed = m_pressed;
    m_pressed = false;
    if (!wasPressed)
        return;
    notifyButtonGroupClicked();
    if (m_checked && buttonGroupExclusive())
        return;                   // 互斥组内不能由用户取消当前选中项 (Qt 语义)
    setChecked(!m_checked);
    sigToggled.emit(m_checked);
}
