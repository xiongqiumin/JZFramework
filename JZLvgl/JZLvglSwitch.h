#ifndef JZ_LVGL_SWITCH_H_
#define JZ_LVGL_SWITCH_H_

#include "JZLvglWidget.h"
#include "JZSignal.h"

class JZLvglSwitch : public JZLvglWidget
{
public:
    JZSigBool sigToggled;
    explicit JZLvglSwitch(JZLvglWidget* parent = nullptr);

    void setState(bool on) { on ? lv_obj_add_state(m_lv, LV_STATE_CHECKED) : lv_obj_remove_state(m_lv, LV_STATE_CHECKED); }
    bool isOn() const       { return lv_obj_has_state(m_lv, LV_STATE_CHECKED); }

protected:
    void onLvEvent(lv_event_t* e) override;
};

#endif
