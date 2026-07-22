#ifndef JZ_LVGL_IMAGE_H_
#define JZ_LVGL_IMAGE_H_

#include "JZWidget.h"
#include "JZImage.h"

class JZImageLabel : public JZWidget
{
    JZ_OBJECT_DECLARE(JZImageLabel)

public:
    explicit JZImageLabel(JZWidget* parent = nullptr);

    void setImage(const JZImage& img);
    JZImage image() const;

    void setScale(uint16_t zoom);
    void setRotation(int32_t angle);
    void setPivot(int x, int y);

private:
    JZImage m_image;
};

#endif
