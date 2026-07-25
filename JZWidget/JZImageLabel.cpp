#include "JZImageLabel.h"

JZ_OBJECT_IMPL(JZImageLabel, JZWidget)

JZImageLabel::JZImageLabel(JZWidget* parent)
    : JZWidget(parent)
{
}

void JZImageLabel::setImage(const JZImage& img)
{}

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

JZSize JZImageLabel::sizeHint() const
{
    int w = m_width > 0 ? m_width : 60;
    int h = m_height > 0 ? m_height : 60;
    return JZSize(w, h);
}