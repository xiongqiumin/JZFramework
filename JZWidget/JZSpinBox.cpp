#include "JZSpinBox.h"
#include "JZPainter.h"
#include "JZFontMetrics.h"

#include <algorithm>
#include <cctype>
#include <string>

JZ_OBJECT_IMPL(JZSpinBox, JZWidget)

namespace {
// 画一个上下按钮的三角箭头 (up=true 朝上)
void drawSpinTri(JZPainter& p, int cx, int cy, bool up)
{
    int s = 4;
    if (up) { p.drawLine(cx, cy - s, cx - s, cy + s); p.drawLine(cx - s, cy + s, cx + s, cy + s); p.drawLine(cx + s, cy + s, cx, cy - s); }
    else    { p.drawLine(cx, cy + s, cx - s, cy - s); p.drawLine(cx - s, cy - s, cx + s, cy - s); p.drawLine(cx + s, cy - s, cx, cy + s); }
}
} // namespace

JZSpinBox::JZSpinBox(JZWidget* parent)
    : JZWidget(parent)
    , sigValueChanged(this)
{}

JZSize JZSpinBox::sizeHint() const
{
    int w = m_width > 0 ? m_width : 90;
    int h = m_height > 0 ? m_height : 30;
    return JZSize(w, h);
}

JZSize JZSpinBox::minimumSizeHint() const
{
    // 数字框: 宽度保留数字 + 按钮最小占位, 高度保留一行下限。
    return JZSize(60, 26);
}

void JZSpinBox::setRange(int min, int max)
{
    m_min = min; m_max = max;
    setValueInternal(m_value);   // 新范围可能夹住当前值
}

void JZSpinBox::setMinimum(int min) { setRange(min, (int)m_max); }
int  JZSpinBox::minimum() const { return (int)m_min; }
void JZSpinBox::setMaximum(int max) { setRange((int)m_min, max); }
int  JZSpinBox::maximum() const { return (int)m_max; }

void JZSpinBox::setStep(int step) { m_step = std::max(1, step); }
int  JZSpinBox::step() const { return (int)m_step; }

void JZSpinBox::setValue(int v) { setValueInternal(v); }

int JZSpinBox::value() const { return (int)m_value; }

void JZSpinBox::setSuffix(const std::string& s) { m_suffix = s; update(); }
std::string JZSpinBox::suffix() const { return m_suffix; }

void JZSpinBox::setSpecialValueText(const std::string& s) { m_special = s; update(); }
std::string JZSpinBox::specialValueText() const { return m_special; }

void JZSpinBox::setReadOnly(bool ro) { m_readOnly = ro; update(); }
bool JZSpinBox::isReadOnly() const { return m_readOnly; }

// ==================== 交给派生类覆盖的钩子 ====================
std::string JZSpinBox::valueText() const
{
    // Qt 语义: 值 == 最小值 且设了 specialValueText 时, 显示那段文本(不含后缀)
    if (!m_special.empty() && m_value == m_min)
        return m_special;
    return std::to_string((int)m_value) + m_suffix;
}

bool JZSpinBox::parseEditText(const std::string& buf, double& out) const
{
    try {
        out = (double)std::stoi(buf);
        return true;
    } catch (...) {
        return false;   // 解析失败: 调用方丢弃缓冲, 回退到当前值
    }
}

bool JZSpinBox::isEditableChar(char c) const
{
    return std::isdigit((unsigned char)c) != 0;
}

void JZSpinBox::emitValueChanged()
{
    sigValueChanged.emit((int)m_value);
}

void JZSpinBox::setValueInternal(double v)
{
    const double nv = std::max(m_min, std::min(v, m_max));
    if (nv == m_value) return;
    m_value = nv;
    emitValueChanged();
    update();
}

// ==================== 编辑缓冲 ====================
void JZSpinBox::commitEdit()
{
    if (!m_editing) return;
    m_editing = false;
    if (!m_editBuffer.empty()) {
        double v = 0;
        if (parseEditText(m_editBuffer, v))
            setValueInternal(v);
        // 解析失败: 丢弃缓冲, 回退到当前值
    }
    m_editBuffer.clear();
    update();
}

void JZSpinBox::cancelEdit()
{
    m_editing = false;
    m_editBuffer.clear();
    update();
}

// ==================== 绘制 ====================
void JZSpinBox::paintEvent(JZPainterEvent*)
{
    JZPainter p(this);
    int w = width(), h = height();
    int btnW = 18;

    // 外框 (聚焦高亮)
    bool focused = hasFocus();
    p.setBrush(JZBrush(JZColor(0xff, 0xff, 0xff)));
    p.setPen(JZPen(focused ? JZColor(0x2A, 0x7F, 0xFF) : JZColor(0xC0, 0xC4, 0xCC),
                   focused ? 2 : 1));
    p.drawRoundRect(0, 0, w, h, 3);

    // 文本 (编辑时显示缓冲; 否则显示值文本), 右对齐在编辑区
    JZFont font(14);
    p.setFont(font);
    std::string txt = m_editing ? m_editBuffer : valueText();
    int tw = JZFontMetrics::textWidth(txt, font);
    int textRight = w - btnW - 6;
    int tx = std::max(6, textRight - tw);
    p.setPen(JZPen(JZColor(0x20, 0x20, 0x20), 1));
    p.drawText(tx, JZFontMetrics::centerY(font, h), txt.c_str());

    // 上下按钮 (竖线分隔 + 三角)
    int upY = 0, upH = h / 2;
    int dnY = upH, dnH = h - upH;
    // 按钮底色: 上按钮按下时加深
    p.fillRect(w - btnW, upY, btnW, upH, m_pressUp ? JZColor(0xd8, 0xd8, 0xd8) : JZColor(0xf2, 0xf2, 0xf2));
    p.fillRect(w - btnW, dnY, btnW, dnH, JZColor(0xf2, 0xf2, 0xf2));
    p.setPen(JZPen(JZColor(0xC0, 0xC4, 0xCC), 1));
    p.drawLine(w - btnW, 0, w - btnW, h);
    p.drawLine(w - btnW, upH, w, upH);

    p.setPen(JZPen(JZColor(0x60, 0x60, 0x60), 1));
    drawSpinTri(p, w - btnW / 2, upH / 2, true);
    drawSpinTri(p, w - btnW / 2, dnY + dnH / 2, false);
}

// ==================== 交互 ====================
int JZSpinBox::hitButton(const JZPoint& p) const
{
    int w = width(), h = height();
    int btnW = 18;
    if (p.x() < w - btnW) return -1;
    return (p.y() < h / 2) ? 0 : 1;   // 0 上, 1 下
}

void JZSpinBox::mousePressEvent(JZMouseEvent* event)
{
    if (event->button() != JZMouseEvent::LeftButton) {
        JZWidget::mousePressEvent(event);
        return;
    }
    setFocus();
    if (m_readOnly)
        return;
    int b = hitButton(event->localPos());
    if (b == 0) { m_pressUp = true; setValueInternal(m_value + m_step); update(); }
    else if (b == 1) { m_pressDown = true; setValueInternal(m_value - m_step); update(); }
}

void JZSpinBox::mouseReleaseEvent(JZMouseEvent* event)
{
    m_pressUp = m_pressDown = false;
    update();
}

void JZSpinBox::wheelEvent(JZMouseEvent* event)
{
    if (m_readOnly) { JZWidget::wheelEvent(event); return; }
    if (event->wheelDelta() != 0) {
        setValueInternal(m_value + (event->wheelDelta() > 0 ? m_step : -m_step));
        event->accept();   // 已消费, 不向父链冒泡
        return;
    }
    JZWidget::wheelEvent(event);
}

void JZSpinBox::keyPressEvent(JZKeyEvent* event)
{
    if (!event->isPress()) {
        JZWidget::keyPressEvent(event);
        return;
    }
    if (m_readOnly) {
        JZWidget::keyPressEvent(event);
        return;
    }

    const std::string& t = event->text();
    if (t.size() == 1 && isEditableChar(t[0])) {
        // '-' 只允许出现在开头(否则会拼出 "--1" 这种)
        if (t[0] == '-' && !m_editBuffer.empty())
            return;
        m_editing = true;
        m_editBuffer += t;
        update();
        return;
    }

    switch (event->key()) {
    case JZKey::Key_Backspace:
    case JZKey::Key_Delete:
        if (m_editing && !m_editBuffer.empty()) {
            m_editBuffer.pop_back();
            update();
        }
        return;
    case JZKey::Key_Return:
        commitEdit();
        return;
    case JZKey::Key_Escape:
        cancelEdit();
        return;
    case JZKey::Key_Up:
        if (m_editing) commitEdit();
        setValueInternal(m_value + m_step);
        return;
    case JZKey::Key_Down:
        if (m_editing) commitEdit();
        setValueInternal(m_value - m_step);
        return;
    case JZKey::Key_Home:
        setValueInternal(m_min);
        return;
    case JZKey::Key_End:
        setValueInternal(m_max);
        return;
    default:
        JZWidget::keyPressEvent(event);
    }
}

void JZSpinBox::focusInEvent(JZEvent* event)
{
    JZWidget::focusInEvent(event);
}

void JZSpinBox::focusOutEvent(JZEvent* event)
{
    commitEdit();   // 失焦提交未确认的编辑
    JZWidget::focusOutEvent(event);
}
