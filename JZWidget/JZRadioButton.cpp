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

JZRadioButton::JZRadioButton(const std::string& text, JZWidget* parent)
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
    // 未入组时: 同一父级下的兄弟单选钮自动互斥 (Qt autoExclusive 风格)。
    // 入了 JZButtonGroup 则由组统一互斥 (见 notifyButtonGroupChecked), 不再看兄弟。
    if (on && !buttonGroup() && parentWidget()) {
        const auto& ch = parentWidget()->children();
        for (auto* obj : ch) {
            auto* r = jzobject_cast<JZRadioButton*>(obj);
            if (r && r != this && r->m_checked) {
                r->m_checked = false;
                r->update();
            }
        }
    }
    update();
    notifyButtonGroupChecked();
}

void JZRadioButton::paintEvent(JZPainterEvent* event)
{
    JZPainter p(this);
    const int d = 18;                       // 指示器外接框 (含 1px 留白)
    const int top = (height() - d) / 2;
    const int cx = 1 + (d - 2) / 2;         // 外圈圆心 x (外圈矩形: x=1, 尺寸 d-2)
    const int cy = top + 1 + (d - 2) / 2;   // 外圈圆心 y

    // 外圈只描边不填内部: drawRoundRect 会用当前 brush 填满整个圆,
    // 默认 brush 是不透明黑, 不显式置透明会把整个指示器涂成实心黑饼。
    p.setBrush(JZBrush(JZColor::Transparent));
    JZColor ring = m_checked ? JZColor(0x33, 0x66, 0xcc)   // 选中: 主题蓝
                 : m_pressed  ? JZColor(0x66, 0x66, 0x66)  // 按住未选中: 加深灰
                              : JZColor(0x99, 0x99, 0x99); // 常态: 灰
    p.setPen(JZPen(ring, 1));
    p.drawRoundRect(1, top + 1, d - 2, d - 2, (d - 2) / 2);

    // 选中: 内圆点 (半径=边长一半的圆角填充 = 真圆, 不再用方点)
    if (m_checked) {
        const int dd = 8;
        p.fillRoundRect(cx - dd / 2, cy - dd / 2, dd, dd, dd / 2,
                        JZColor(0x33, 0x66, 0xcc));
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

JZSize JZRadioButton::minimumSizeHint() const
{
    return JZSize(0, 22);
}

// ==================== 鼠标交互 ====================
// 左键按下 + 本控件内释放 -> 选中 (已是选中态则不重复发信号)
void JZRadioButton::mousePressEvent(JZMouseEvent* event)
{
    if (event->button() == JZMouseEvent::LeftButton)
        m_pressed = true;
}

void JZRadioButton::mouseReleaseEvent(JZMouseEvent* event)
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
    if (m_checked)
        return;                   // 互斥组内点击已选中项: 保持选中 (Qt 语义)
    setChecked(true);
    sigToggled.emit();
}
