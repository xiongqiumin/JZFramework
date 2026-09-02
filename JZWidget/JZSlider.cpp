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

    // 滑轨
    p.setBrush(JZBrush(JZColor(0xcc, 0xcc, 0xcc)));
    p.setPen(JZPen(JZColor(0xbb, 0xbb, 0xbb), 1));
    p.drawRoundRect(2, ty, w - 4, trackH, trackH / 2);

    // 滑块位置 + 已填充部分 (value 左侧高亮)
    int range = m_max - m_min;
    int pos = (range > 0) ? (w - 12) * (m_value - m_min) / range + 6 : w / 2;
    if (pos > 6) {
        p.setBrush(JZBrush(JZColor(0x33, 0x66, 0xcc)));
        p.setPen(JZPen(JZColor(0x33, 0x66, 0xcc), 1));
        p.drawRoundRect(6, ty, pos - 6, trackH, trackH / 2);
    }

    // 滑块 (白底蓝边圆点)
    int knob = 12;
    p.setBrush(JZBrush(JZColor(0xff, 0xff, 0xff)));
    p.setPen(JZPen(JZColor(0x33, 0x66, 0xcc), 1));
    p.drawRoundRect(pos - knob / 2, (h - knob) / 2, knob, knob, knob / 2);
}

// ==================== 鼠标交互 ====================
// 命中位置 -> 值: 滑块中心对齐, 与 paintEvent 的 6px 内边距一致
int JZSlider::valueFromPos(int x) const
{
    int range = m_max - m_min;
    int span = width() - 12;
    if (range <= 0 || span <= 0)
        return m_value;
    int pos = std::max(6, std::min(x, width() - 6));
    return m_min + (int32_t)((int64_t)(pos - 6) * range / span);
}

void JZSlider::mousePressEvent(JZMouseEvent* event)
{
    if (event->button() != JZMouseEvent::LeftButton) {
        JZWidget::mousePressEvent(event);
        return;
    }
    m_dragging = true;
    setValue(valueFromPos(event->localPos().x()));
    update();
    sigValueChanged.emit(m_value);
}

void JZSlider::mouseMoveEvent(JZMouseEvent* event)
{
    if (!m_dragging)
        return;   // 未按下时的悬停移动不改变值
    setValue(valueFromPos(event->localPos().x()));
    update();
    sigValueChanged.emit(m_value);
}

void JZSlider::mouseReleaseEvent(JZMouseEvent* event)
{
    m_dragging = false;
}
