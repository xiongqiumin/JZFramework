#ifndef JZ_WIDGET_ROLLER_H_
#define JZ_WIDGET_ROLLER_H_

#include "JZWidget.h"

class JZRoller : public JZWidget
{
    JZ_OBJECT_DECLARE(JZRoller)

public:
    explicit JZRoller(JZWidget* parent = nullptr);

    void setOptions(const std::string& opts, lv_roller_mode_t mode = LV_ROLLER_MODE_NORMAL);
    void setVisibleRowCount(uint8_t n);
    void setSelected(uint32_t idx, bool anim = false);
    uint32_t selected() const;
};

#endif
