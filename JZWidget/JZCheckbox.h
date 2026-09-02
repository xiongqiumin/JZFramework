#ifndef JZ_WIDGET_CHECKBOX_H_
#define JZ_WIDGET_CHECKBOX_H_

#include "JZWidget.h"
#include "JZSignal.h"

class JZCheckbox : public JZWidget
{
    JZ_OBJECT_DECLARE(JZCheckbox)

public:
    explicit JZCheckbox(JZWidget* parent = nullptr);

    JZSize sizeHint() const override;
    void setText(const std::string& text);
    std::string text() const;
    bool isChecked() const;
    void setChecked(bool on);

    // ---- AI 语义 ----
    std::string aiText() const override { return m_text; }
    bool aiChecked() const override { return m_checked; }
    std::vector<JZAiAction> aiActions() const override { return {{"click", ""}}; }

jz_signals:
    JZSigBool sigToggled;

protected:
    void paintEvent(JZPainterEvent* event) override;
    void mousePressEvent(JZMouseEvent* event) override;
    void mouseReleaseEvent(JZMouseEvent* event) override;

private:
    std::string m_text;
    bool m_checked = false;
    bool m_pressed = false;
};

#endif
