#ifndef JZ_WIDGET_SLIDER_H_
#define JZ_WIDGET_SLIDER_H_

#include "JZWidget.h"
#include "JZSignal.h"

class JZSlider : public JZWidget
{
    JZ_OBJECT_DECLARE(JZSlider)

public:
    explicit JZSlider(JZWidget* parent = nullptr);

    JZSize sizeHint() const override;
    void setRange(int32_t min, int32_t max);
    void setValue(int32_t v);
    int32_t value() const;

jz_signals:
    JZSigInt sigValueChanged;

protected:
};

#endif
