#include "JZImageLabel.h"

JZ_OBJECT_IMPL(JZImageLabel, JZWidget)

JZImageLabel::JZImageLabel(JZWidget* parent)
    : JZWidget(lv_image_create, parent)
{
}

JZImageLabel::JZImageLabel(JZWidget* parent, const JZImage& img)
    : JZImage(parent)
{
    setImage(img);
}

void JZImageLabel::setImage(const JZImage& img)
{
    m_image = img;
    lv_image_set_src(m_lv, img.dsc());
}

JZImageLabel JZImageLabel::image() const
{
    return m_image;
}

void JZImageLabel::setScale(uint16_t zoom)
{
    lv_image_set_scale(m_lv, zoom);
}

void JZImageLabel::setRotation(int32_t angle)
{
    lv_image_set_rotation(m_lv, angle);
}

void JZImageLabel::setPivot(int x, int y)
{
    lv_image_set_pivot(m_lv, x, y);
}