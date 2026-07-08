#ifndef JZ_LVGL_SLIDER_H_
#define JZ_LVGL_SLIDER_H_

#include "JZWidget.h"
#include "JZSignal.h"

class JZSlider : public JZWidget
{
    JZ_OBJECT_DECLARE(JZSlider)

public:
    explicit JZSlider(JZWidget* parent = nullptr);

    void setRange(int32_t min, int32_t max);
    void setValue(int32_t v);
    int32_t value() const;

jz_signals:
    JZSigInt sigValueChanged;

protected:
    void onLvEvent(lv_event_t* e) override;
};

#endif
