#include "JZPushButton.h"
#include "JZPainter.h"
#include <algorithm>
#include "JZStylePainter.h"
#include "JZFontMetrics.h"

JZ_OBJECT_IMPL(JZPushButton, JZWidget)

JZPushButton::JZPushButton(JZWidget* parent)
    : JZWidget(parent)
    , sigClicked(this)
{}

JZPushButton::JZPushButton(JZWidget* parent, int w, int h, const std::string& text)
    : JZPushButton(parent)
{
    resize(w, h);
    setText(text);
}

void* JZPushButton::labelHandle() const
{
    return m_label;
}

void JZPushButton::setText(const std::string& text)
{
    m_text = text;
}

std::string JZPushButton::text() const
{ return m_text; }

void JZPushButton::setIcon(const JZImage& icon)
{
    m_icon = icon;
    update();
}

const JZImage& JZPushButton::icon() const
{ return m_icon; }

void JZPushButton::setWidth(int w)
{
    resize(w, height());
}

void JZPushButton::setHeight(int h)
{
    resize(width(), h);
}

JZSize JZPushButton::sizeHint() const
{
    int w = m_width > 0 ? m_width : 90;
    int h = m_height > 0 ? m_height : 30;
    if (!m_icon.isNull())
    {
        const int iconPad = 4;
        if (m_width <= 0)
            w += m_icon.width() + (m_text.empty() ? 0 : iconPad);
        if (m_height <= 0)
            h = std::max(h, m_icon.height() + 8);
    }
    return JZSize(w, h);
}

JZSize JZPushButton::minimumSizeHint() const
{
    // 按钮最小尺寸: 宽度可收缩到 0 (文本裁切), 高度保留一行按钮下限。
    return JZSize(0, 24);
}

JZStyleSet JZPushButton::uaStyle() const
{
    // 与改造前硬编码完全一致的默认外观: #e0e0e0 底 / #808080 1px 边框 / 4px 圆角 / #202020 字
    JZStyleSet s;
    s.background = JZOptional<JZColor>::Some(JZColor(0xe0, 0xe0, 0xe0));
    s.setBorder(JZColor(0x80, 0x80, 0x80), 1);
    s.radius = 4;
    s.color = JZOptional<JZColor>::Some(JZColor(0x20, 0x20, 0x20));
    return s;
}

std::vector<std::string> JZPushButton::styleProps() const
{
    std::vector<std::string> v = JZWidget::styleProps();
    v.push_back("color");   // 文本颜色
    return v;
}

void JZPushButton::paintEvent(JZPainterEvent* event)
{
    JZPainter p(this);

    JZStyleSet s = style();
    // 按下反馈: 底色压暗 (不写死颜色 —— 任意主题下都成立;
    // 默认主题下 #e0e0e0 * 0.84 ≈ #bcbcbc, 与原硬编码 #c0c0c0 基本一致)
    if (m_pressed && s.background.isSome())
    {
        JZColor c = s.background.value();
        s.background = JZOptional<JZColor>::Some(JZColor(
            (uint8_t)(c.red() * 0.84), (uint8_t)(c.green() * 0.84),
            (uint8_t)(c.blue() * 0.84), c.alpha()));
    }

    // 盒子层 (背景/边框/圆角) 由样式驱动
    JZStylePainter::drawBox(p, s, rect());

    // 内容层: 图标 + 文本, 基于 contentRect (border + padding 内缩)
    JZRect cr = JZStylePainter::contentRect(s, rect());
    JZFont font(14);
    p.setPen(JZPen(s.color.valueOr(JZColor(0x20, 0x20, 0x20)), 1));
    p.setFont(font);

    const int iconPad = 4;
    int iconW = m_icon.isNull() ? 0 : m_icon.width();
    int iconH = m_icon.isNull() ? 0 : m_icon.height();

    if (!m_icon.isNull())
    {
        if (m_text.empty())
        {
            // 纯图标按钮: 图标在整个内容区居中
            int ix = cr.x() + (cr.width() - iconW) / 2;
            int iy = cr.y() + (cr.height() - iconH) / 2;
            p.drawImage(m_icon, ix, iy, iconW, iconH);
        }
        else
        {
            // 图文并排: 图标靠左, 文本紧随其后
            int iy = cr.y() + (cr.height() - iconH) / 2;
            p.drawImage(m_icon, cr.x(), iy, iconW, iconH);
            std::string t = m_text;
            int tx = cr.x() + iconW + iconPad;
            int ty = cr.y() + JZFontMetrics::centerY(font, cr.height());
            p.drawText(tx + JZFontMetrics::centerX(t, font, cr.width() - iconW - iconPad),
                       ty, t.c_str());
        }
    }
    else if (!m_text.empty())
    {
        p.drawText(cr.x() + JZFontMetrics::centerX(m_text, font, cr.width()),
                   cr.y() + JZFontMetrics::centerY(font, cr.height()), m_text.c_str());
    }
}

void JZPushButton::mousePressEvent(JZMouseEvent* event)
{
    m_pressed = true;
    update();
}

void JZPushButton::mouseReleaseEvent(JZMouseEvent* event)
{
    if (m_pressed)
        sigClicked.emit();
    m_pressed = false;
    update();
}
