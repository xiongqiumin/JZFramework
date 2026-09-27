#ifndef JZ_WIDGET_BAR_H_
#define JZ_WIDGET_BAR_H_
#include "JZWidget.h"

class JZFRAMEWORK_API JZProgressBar : public JZWidget
{
    JZ_OBJECT_DECLARE(JZProgressBar)

public:
    explicit JZProgressBar(JZWidget* parent = nullptr);

    JZSize sizeHint() const override;
    void setRange(int32_t min, int32_t max);
    void setValue(int32_t v, bool anim = false);
    int32_t value() const;

protected:
    void paintEvent(JZPainterEvent* event) override;

private:
    int32_t m_min = 0;
    int32_t m_max = 100;
    int32_t m_value = 0;
};

#endif
