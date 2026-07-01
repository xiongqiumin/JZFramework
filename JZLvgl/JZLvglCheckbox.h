#ifndef JZ_LVGL_CHECKBOX_H_
#define JZ_LVGL_CHECKBOX_H_

#include "JZLvglWidget.h"
#include "JZSignal.h"

class JZLvglCheckbox : public JZLvglWidget
{
public:
    JZSigBool sigToggled;
    explicit JZLvglCheckbox(JZLvglWidget* parent = nullptr);

    void setText(const std::string& text);
    std::string text() const;
    bool isChecked() const;
};

#endif
