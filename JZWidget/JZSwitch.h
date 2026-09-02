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

    // ---- AI 语义 ----
    bool aiChecked() const override { return m_on; }
    std::vector<JZAiAction> aiActions() const override { return {{"click", ""}}; }

jz_signals:
    JZSigBool sigToggled;

protected:
    void paintEvent(JZPainterEvent* event) override;
    void mousePressEvent(JZMouseEvent* event) override;
    void mouseReleaseEvent(JZMouseEvent* event) override;

private:
    bool m_on = false;
    bool m_pressed = false;
};

#endif
