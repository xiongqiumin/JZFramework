#include "JZLabel.h"
#include "JZPainter.h"
#include "JZStylePainter.h"
#include "JZFontMetrics.h"
#include "JZDebug.h"

JZ_OBJECT_IMPL(JZLabel, JZWidget)

JZLabel::JZLabel(JZWidget* parent)
    : JZWidget(parent)
{}

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
    return JZSize(std::max(80,tw + 12), std::max(20,th + 8));
}

JZSize JZLabel::minimumSizeHint() const
{
    // 标签最小尺寸: 宽度可收缩到 0 (文本裁切/省略), 高度保留一行文本下限。
    return JZSize(0, 16);
}

void JZLabel::paintEvent(JZPainterEvent* event)
{
    JZPainter p(this);
    const JZStyleSet& s = style();

    // 盒子层: 背景 / 边框 / 圆角 (UA 默认无背景无边框, 空转)
    JZStylePainter::drawBox(p, s, rect());

    if (m_text.empty())
        return;

    // 内容层: 文本基于 contentRect (border + padding 内缩)
    JZRect cr = JZStylePainter::contentRect(s, rect());
    JZFont font(14);
    p.setPen(JZPen(s.color.valueOr(JZColor(0x33, 0x33, 0x33)), 1));
    p.setFont(font);

    int tw = JZFontMetrics::textWidth(m_text, font);

    // 对齐: 0=左 1=中 2=右; 控件被压扁时垂直顶对齐而非负偏移
    int tx = (m_align == 1) ? cr.x() + (cr.width() - tw) / 2
           : (m_align == 2) ? cr.x() + cr.width() - tw
                            : cr.x();
    int ty = cr.y() + JZFontMetrics::centerY(font, cr.height());
    if (ty < cr.y())
        ty = cr.y();
    p.drawText(tx, ty, m_text.c_str());
}

JZStyleSet JZLabel::uaStyle() const
{
    JZStyleSet s;
    s.color = JZOptional<JZColor>::Some(JZColor(0x33, 0x33, 0x33));
    // 原来硬编码的"左右各留 6px" (与 sizeHint 的 +12 一致) 显式化为 padding,
    // 使无样式表时的排版与改造前逐像素一致, 同时 padding 可被样式表覆盖。
    s.paddingLeft = 6;
    s.paddingRight = 6;
    return s;
}
