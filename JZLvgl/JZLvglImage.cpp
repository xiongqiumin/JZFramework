#include "JZLvglImage.h"

JZLvglImage::JZLvglImage(JZLvglWidget* parent)
    : JZLvglWidget(lv_image_create, parent)
{
}

JZLvglImage::JZLvglImage(JZLvglWidget* parent, const JZImage& img)
    : JZLvglImage(parent)
{
    setImage(img);
}

void JZLvglImage::setImage(const JZImage& img)
{
    m_image = img;
    lv_image_set_src(m_lv, img.dsc());
}
