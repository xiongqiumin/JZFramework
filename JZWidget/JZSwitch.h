#ifndef JZ_WIDGET_SWITCH_H_
#define JZ_WIDGET_SWITCH_H_

#include "JZWidget.h"
#include "JZSignal.h"

class JZSwitch : public JZWidget
{
    JZ_OBJECT_DECLARE(JZSwitch)

public:
    explicit JZSwitch(JZWidget* parent = nullptr);

    JZSize sizeHint() const override;
    void setState(bool on);
    bool isOn() const;

jz_signals:
    JZSigBool sigToggled;

protected:
};

#endif
