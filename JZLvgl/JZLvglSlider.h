#ifndef JZ_LVGL_SLIDER_H_
#define JZ_LVGL_SLIDER_H_

#include "JZLvglWidget.h"
#include "JZSignal.h"

class JZLvglSlider : public JZLvglWidget
{
public:
    JZSigInt sigValueChanged;
    explicit JZLvglSlider(JZLvglWidget* parent = nullptr);

    void setRange(int32_t min, int32_t max) { lv_slider_set_range(m_lv, min, max); }
    void setValue(int32_t v)                { lv_slider_set_value(m_lv, v, LV_ANIM_OFF); }
    int32_t value() const                   { return lv_slider_get_value(m_lv); }

protected:
    void onLvEvent(lv_event_t* e) override;
};

#endif
