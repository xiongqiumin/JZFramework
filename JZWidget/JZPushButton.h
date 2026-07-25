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
    void* labelHandle() const;

    void setText(const std::string& text);
    std::string text() const;

    void setWidth(int w);
    void setHeight(int h);

jz_signals:
    JZSigVoid sigClicked;

protected:
    void paintEvent(JZPainterEvent* event) override;

private:
    void* m_label = nullptr;
    std::string m_text;
};

#endif
