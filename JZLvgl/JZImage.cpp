#include "JZImage.h"

JZImage::JZImage(const lv_image_dsc_t* dsc)
    : m_dsc(*dsc)
{
}

void JZImage::setDsc(const lv_image_dsc_t* dsc)
{
    if (dsc) m_dsc = *dsc;
}

const lv_image_dsc_t* JZImage::dsc() const
{
    return &m_dsc;
}

int JZImage::width() const
{
    return m_dsc.header.w;
}

int JZImage::height() const
{
    return m_dsc.header.h;
}
