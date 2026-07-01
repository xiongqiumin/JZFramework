#ifndef JZ_LVGL_IMAGE_H_
#define JZ_LVGL_IMAGE_H_

#include "JZLvglWidget.h"
#include "JZImage.h"

class JZLvglImage : public JZLvglWidget
{
public:
    explicit JZLvglImage(JZLvglWidget* parent = nullptr);
    JZLvglImage(JZLvglWidget* parent, const JZImage& img);

    void setImage(const JZImage& img);
    JZImage image() const               { return m_image; }

    void setScale(uint16_t zoom)         { lv_image_set_scale(m_lv, zoom); }
    void setRotation(int32_t angle)      { lv_image_set_rotation(m_lv, angle); }
    void setPivot(int x, int y)          { lv_image_set_pivot(m_lv, x, y); }

    void setSrcBySymbol(const char* sym) { lv_image_set_src(m_lv, sym); }

private:
    JZImage m_image;
};

#endif
