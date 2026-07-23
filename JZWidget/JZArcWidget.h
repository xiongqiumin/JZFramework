#ifndef JZ_WIDGET_ARC_H_
#define JZ_WIDGET_ARC_H_

#include "JZWidget.h"
#include "JZSignal.h"

class JZArcWidget : public JZWidget
{
    JZ_OBJECT_DECLARE(JZArcWidget)

public:
    explicit JZArcWidget(JZWidget* parent = nullptr);

    void setRange(int32_t min, int32_t max);
    void setValue(int32_t v);
    int32_t value() const;
    void setAngles(uint16_t bgStart, uint16_t bgEnd);
    void setRotation(uint16_t rot);

jz_signals:
    JZSigInt sigValueChanged;

protected:
    void onLvEvent(lv_event_t* e) override;
};

#endif
