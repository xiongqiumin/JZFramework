#ifndef JZ_IMAGE_H_
#define JZ_IMAGE_H_

#include "lvgl.h"

class JZImage
{
public:
    JZImage() = default;
    explicit JZImage(const lv_image_dsc_t* dsc);

    void setDsc(const lv_image_dsc_t* dsc);
    const lv_image_dsc_t* dsc() const;

    int width() const;
    int height() const;

private:
    lv_image_dsc_t m_dsc = {};
};

#endif
