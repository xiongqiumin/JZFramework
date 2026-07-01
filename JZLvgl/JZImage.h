#ifndef JZ_IMAGE_H_
#define JZ_IMAGE_H_

#include "lvgl.h"

class JZImage
{
public:
    JZImage() = default;
    explicit JZImage(const lv_image_dsc_t* dsc) : m_dsc(*dsc) {}

    void setDsc(const lv_image_dsc_t* dsc)  { if (dsc) m_dsc = *dsc; }
    const lv_image_dsc_t* dsc() const       { return &m_dsc; }

    int width() const   { return m_dsc.header.w; }
    int height() const  { return m_dsc.header.h; }

private:
    lv_image_dsc_t m_dsc = {};
};

#endif
