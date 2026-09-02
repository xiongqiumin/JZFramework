#include "JZSwitch.h"
#include "JZPainter.h"

JZ_OBJECT_IMPL(JZSwitch, JZWidget)

JZSwitch::JZSwitch(JZWidget* parent)
    : JZWidget(parent)
    , sigToggled(this)
{
}

void JZSwitch::setState(bool on)
{
    m_on = on;
}

bool JZSwitch::isOn() const
{ return m_on; }

JZSize JZSwitch::sizeHint() const
{
    return JZSize(52, 26);
}

void JZSwitch::paintEvent(JZPainterEvent* event)
{
    JZPainter p(this);
    int w = width(), h = height();

    int r = h / 2;
    p.setBrush(JZBrush(m_on ? JZColor(0x33, 0x99, 0x33) : JZColor(0xcc, 0xcc, 0xcc)));
    p.setPen(JZPen(JZColor(0xaa, 0xaa, 0xaa), 1));
    p.drawRoundRect(0, 0, w, h, r);

    int knob = h - 4;
    int kx = m_on ? (w - knob - 2) : 2;
    p.setBrush(JZBrush(JZColor(0xff, 0xff, 0xff)));
    p.setPen(JZPen(JZColor(0xaa, 0xaa, 0xaa), 1));
    p.drawRoundRect(kx, 2, knob, knob, knob / 2);
}

// ==================== 鼠标交互 ====================
// 左键按下 + 本控件内释放 -> 切换状态 (Qt 按钮语义)
void JZSwitch::mousePressEvent(JZMouseEvent* event)
{
    if (event->button() == JZMouseEvent::LeftButton)
        m_pressed = true;
}

void JZSwitch::mouseReleaseEvent(JZMouseEvent* event)
{
    if (event->button() != JZMouseEvent::LeftButton) {
        JZWidget::mouseReleaseEvent(event);
        return;
    }
    bool wasPressed = m_pressed;
    m_pressed = false;
    if (!wasPressed)
        return;   // 释放前不是本控件按下 (如别处拖入), 不切换
    m_on = !m_on;
    update();
    sigToggled.emit(m_on);
}
