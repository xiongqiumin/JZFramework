#include "JZImageLabel.h"
#include "JZPainter.h"

JZ_OBJECT_IMPL(JZImageLabel, JZWidget)

JZImageLabel::JZImageLabel(JZWidget* parent)
    : JZWidget(parent)
{
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
    int w = width(), h = height();

    if (m_image.data()) {
        p.drawImage(m_image, 0, 0, w, h);
    } else {
        // 无图像: 占位框
        p.setBrush(JZBrush(JZColor(0xee, 0xee, 0xee)));
        p.setPen(JZPen(JZColor(0xaa, 0xaa, 0xaa), 1));
        p.drawRect(0, 0, w, h);
    }
}
