#ifndef JZ_WIDGET_RADIO_BUTTON_H_
#define JZ_WIDGET_RADIO_BUTTON_H_

#include "JZWidget.h"
#include "JZSignal.h"

class JZRadioButton : public JZWidget
{
    JZ_OBJECT_DECLARE(JZRadioButton)

public:
    explicit JZRadioButton(JZWidget* parent = nullptr);
    JZRadioButton(JZWidget* parent, const std::string& text);

    JZSize sizeHint() const override;
    void setText(const std::string& text);
    std::string text() const;

    bool isChecked() const;
    void setChecked(bool on);

jz_signals:
    JZSigVoid sigToggled;   // 选中时触发

protected:
    void paintEvent(JZPainterEvent* event) override;

    void* m_label = nullptr;
    void* m_indicator = nullptr;   // 透明占位, 为圆环预留布局宽度
    bool m_checked = false;
    std::string m_text;
};

#endif
