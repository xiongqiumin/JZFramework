#ifndef JZ_LVGL_BAR_H_
#define JZ_LVGL_BAR_H_

#include "JZWidget.h"

class JZProgressBar : public JZWidget
{
    JZ_OBJECT_DECLARE(JZProgressBar)

public:
    explicit JZProgressBar(JZWidget* parent = nullptr);

    void setRange(int32_t min, int32_t max);
    void setValue(int32_t v, bool anim = false);
    int32_t value() const;
};

#endif
