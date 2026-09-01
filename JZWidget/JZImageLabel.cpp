#include "JZImageLabel.h"
#include "JZPainter.h"
#include "JZStylePainter.h"

JZ_OBJECT_IMPL(JZImageLabel, JZWidget)

JZImageLabel::JZImageLabel(JZWidget* parent)
    : JZWidget(parent)
{
    // 默认外观: 占位底色 + 细边框 (原硬编码迁移到样式)
    style().background = JZOptional<JZColor>::Some(JZColor(0xee, 0xee, 0xee));
    style().borderColor = JZOptional<JZColor>::Some(JZColor(0xaa, 0xaa, 0xaa));
    style().borderWidth = 1;
}

JZSize JZImageLabel::sizeHint() const
{
    int w = m_width > 0 ? m_width : 60;
    int h = m_height > 0 ? m_height : 60;
    return JZSize(w, h);
}

void JZImageLabel::setImage(const JZImage& img)
{
    m_image = img;
}

JZImage JZImageLabel::image() const
{
    return m_image;
}

void JZImageLabel::setScale(uint16_t zoom)
{}

void JZImageLabel::setRotation(int32_t angle)
{}

void JZImageLabel::setPivot(int x, int y)
{}

void JZImageLabel::paintEvent(JZPainterEvent* event)
{
    JZPainter p(this);

    // 样式盒子 (背景/边框) 由控件主动绘制, 内容定位在样式内容区内
    JZStylePainter::drawBox(p, style(), rect());
    JZRect content = JZStylePainter::contentRect(style(), rect());

    if (m_image.data())
        p.drawImage(m_image, content.x(), content.y(), content.width(), content.height());
}
