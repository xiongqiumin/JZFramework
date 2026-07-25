#ifndef JZ_WIDGET_LABEL_H_
#define JZ_WIDGET_LABEL_H_

#include "JZWidget.h"

class JZLabel : public JZWidget
{
    JZ_OBJECT_DECLARE(JZLabel)

public:
    explicit JZLabel(JZWidget* parent = nullptr);
    JZLabel(JZWidget* parent, const std::string& text);

    JZSize sizeHint() const override;
    void setText(const std::string& text);
    std::string text() const;

    void setAlign(int align);

protected:
    void paintEvent(JZPainterEvent* event) override;

private:
    std::string m_text;
    int m_align = 0;   // 0=左, 1=中, 2=右
};

#endif
