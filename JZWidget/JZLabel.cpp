#include "JZLabel.h"
#include "JZPainter.h"
#include "JZFontMetrics.h"
#include "JZDebug.h"

JZ_OBJECT_IMPL(JZLabel, JZWidget)

JZLabel::JZLabel(JZWidget* parent)
    : JZWidget(parent)
{
}

JZLabel::JZLabel(const std::string& text, JZWidget* parent)
    : JZLabel(parent)
{
    setText(text);
}

void JZLabel::setText(const std::string& text)
{
    if (m_text == text)
        return;
    m_text = text;
    // 文本变化会改变 sizeHint, 必须通知父布局重排 (重新按新 sizeHint 给本控件
    // 定几何), 否则控件仍停留在旧几何上, 新文本因尺寸不足被裁切。等价 Qt 的
    // QLabel::setText 触发 resize。update() 同步重绘新文本。
    updateGeometry();
    update();
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
    // 按文本内容给期望尺寸 (空文本给最小占位)。
    // 预留左右/上下内边距, 吸收度量误差或字体微调, 防止文字被裁切。
    if (m_text.empty())
        return JZSize(80, 20);
    JZFont font(14);
    int tw = JZFontMetrics::textWidth(m_text, font);
    int th = JZFontMetrics::textHeight(font);
    return JZSize(tw + 12, th + 8);
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
    
    // 左右各留 6px, 与 sizeHint 的 +12 一致; 控件被压扁时垂直顶对齐而非负偏移
    int tx = (m_align == 1) ? (w - tw) / 2 : (m_align == 2) ? (w - tw - 6) : 6;
    int ty = JZFontMetrics::centerY(font, h);
    if (ty < 0)
        ty = 0;
    p.drawText(tx, ty, m_text.c_str());
}
