#ifndef JZ_WIDGET_RADIO_BUTTON_H_
#define JZ_WIDGET_RADIO_BUTTON_H_
#include "JZWidget.h"
#include "JZSignal.h"

class JZFRAMEWORK_API JZRadioButton : public JZWidget
{
    JZ_OBJECT_DECLARE(JZRadioButton)

public:
    explicit JZRadioButton(JZWidget* parent = nullptr);
    JZRadioButton(const std::string& text, JZWidget* parent = nullptr);

    JZSize sizeHint() const override;
    JZSize minimumSizeHint() const override;
    void setText(const std::string& text);
    std::string text() const;

    bool isChecked() const;
    void setChecked(bool on);

    // ---- AI 语义 ----
    std::string aiText() const override { return m_text; }
    bool aiChecked() const override { return m_checked; }
    std::vector<JZAiAction> aiActions() const override { return {{"click", ""}}; }

jz_signals:
    JZSigVoid sigToggled;   // 选中时触发

protected:
    void paintEvent(JZPainterEvent* event) override;
    void mousePressEvent(JZMouseEvent* event) override;
    void mouseReleaseEvent(JZMouseEvent* event) override;

    bool m_checked = false;
    bool m_pressed = false;
    std::string m_text;
};

#endif
