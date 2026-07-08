#ifndef JZ_LVGL_RADIO_BUTTON_H_
#define JZ_LVGL_RADIO_BUTTON_H_

#include "JZWidget.h"
#include "JZSignal.h"

class JZRadioButton : public JZWidget
{
    JZ_OBJECT_DECLARE(JZRadioButton)

public:
    explicit JZRadioButton(JZWidget* parent = nullptr);
    JZRadioButton(JZWidget* parent, const std::string& text);

    void setText(const std::string& text);
    std::string text() const;

    bool isChecked() const;
    void setChecked(bool on);

jz_signals:
    JZSigVoid sigToggled;   // 选中时触发

protected:
    void onLvEvent(lv_event_t* e) override;
};

#endif
