#ifndef JZ_LVGL_ARC_H_
#define JZ_LVGL_ARC_H_

#include "JZLvglWidget.h"
#include "JZSignal.h"

class JZLvglArc : public JZLvglWidget
{
public:
    JZSigInt sigValueChanged;
    explicit JZLvglArc(JZLvglWidget* parent = nullptr);

    void setRange(int32_t min, int32_t max) { lv_arc_set_range(m_lv, min, max); }
    void setValue(int32_t v)                { lv_arc_set_value(m_lv, v); }
    int32_t value() const                   { return lv_arc_get_value(m_lv); }
    void setAngles(uint16_t bgStart, uint16_t bgEnd) { lv_arc_set_bg_angles(m_lv, bgStart, bgEnd); }
    void setRotation(uint16_t rot)          { lv_arc_set_rotation(m_lv, rot); }

protected:
    void onLvEvent(lv_event_t* e) override;
};

#endif
