#ifndef JZ_LVGL_DROPDOWN_H_
#define JZ_LVGL_DROPDOWN_H_

#include "JZLvglWidget.h"
#include "JZSignal.h"

class JZLvglDropdown : public JZLvglWidget
{
public:
    JZSigInt sigValueChanged;
    explicit JZLvglDropdown(JZLvglWidget* parent = nullptr);

    void setOptions(const std::string& opts) { lv_dropdown_set_options(m_lv, opts.c_str()); }
    void setSelected(uint16_t idx)           { lv_dropdown_set_selected(m_lv, idx); }
    uint16_t selected() const                { return lv_dropdown_get_selected(m_lv); }
    void getSelectedStr(char* buf, size_t len) const { lv_dropdown_get_selected_str(m_lv, buf, (uint32_t)len); }

protected:
    void onLvEvent(lv_event_t* e) override;
};

#endif
