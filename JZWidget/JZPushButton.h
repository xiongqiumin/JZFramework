#ifndef JZ_WIDGET_BUTTON_H_
#define JZ_WIDGET_BUTTON_H_

#include "JZWidget.h"
#include "JZSignal.h"

class JZPushButton : public JZWidget
{
    JZ_OBJECT_DECLARE(JZPushButton)

public:
    explicit JZPushButton(JZWidget* parent = nullptr);
    JZPushButton(JZWidget* parent, int w, int h, const std::string& text);

    JZSize sizeHint() const override;
    JZSize minimumSizeHint() const override;
    void* labelHandle() const;

    void setText(const std::string& text);
    std::string text() const;

    void setWidth(int w);
    void setHeight(int h);

    // ---- AI 语义 ----
    std::string aiRole() const override { return "button"; }
    std::string aiText() const override { return m_text; }
    std::vector<JZAiAction> aiActions() const override { return {{"click", ""}}; }

jz_signals:
    JZSigVoid sigClicked;

protected:
    void paintEvent(JZPainterEvent* event) override;
    void mousePressEvent(JZMouseEvent* event) override;
    void mouseReleaseEvent(JZMouseEvent* event) override;

private:
    void* m_label = nullptr;
    std::string m_text;
    bool m_pressed = false;
};

#endif
