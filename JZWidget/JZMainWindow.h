#ifndef JZ_WIDGET_WINDOW_H_
#define JZ_WIDGET_WINDOW_H_

#include "JZWidget.h"

class JZMainWindow : public JZWidget
{
    JZ_OBJECT_DECLARE(JZMainWindow)

public:
    explicit JZMainWindow(JZWidget* parent = nullptr);

protected:
    void paintEvent(JZPainterEvent* event) override;
};

#endif
