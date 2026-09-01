#include "JZMainWindow.h"
#include "JZPainter.h"

JZ_OBJECT_IMPL(JZMainWindow, JZWidget)

JZMainWindow::JZMainWindow(JZWidget* parent)
    : JZWidget(parent)
{
}

void JZMainWindow::paintEvent(JZPainterEvent* event)
{
    JZPainter p(this);
    int w = width(), h = height();
    p.setBrush(JZBrush(JZColor(0xf0, 0xf0, 0xf0)));
    p.setPen(JZPen(JZColor(0xf0, 0xf0, 0xf0), 1));
    p.drawRect(0, 0, w, h);
}
