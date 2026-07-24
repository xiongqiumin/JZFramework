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