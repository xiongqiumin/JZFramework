#ifndef JZ_WIDGET_IMAGE_H_
#define JZ_WIDGET_IMAGE_H_
#include "JZWidget.h"
#include "JZImage.h"

class JZFRAMEWORK_API JZImageLabel : public JZWidget
{
    JZ_OBJECT_DECLARE(JZImageLabel)

public:
    explicit JZImageLabel(JZWidget* parent = nullptr);

    JZSize sizeHint() const override;
    void setImage(const JZImage& img);
    JZImage image() const;

    void setScale(uint16_t zoom);
    void setRotation(int32_t angle);
    void setPivot(int x, int y);

protected:
    void paintEvent(JZPainterEvent* event) override;

private:
    JZImage m_image;
};

#endif
