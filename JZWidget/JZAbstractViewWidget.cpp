#include "JZAbstractViewWidget.h"

#include "JZResizeEvent.h"

JZ_OBJECT_IMPL(JZAbstractViewWidget, JZScrollArea)

JZAbstractViewWidget::JZAbstractViewWidget(JZWidget* parent)
    : JZScrollArea(parent)
{
}

void JZAbstractViewWidget::ensureVisible(int top, int bottom)
{
    if (bottom > scrollY() + viewportHeight())
        scrollToY(bottom - viewportHeight());
    else if (top < scrollY())
        scrollToY(top);
}

void JZAbstractViewWidget::resizeEvent(JZResizeEvent* event)
{
    // 先让基类重摆滚动条 (视口尺寸会因滚动条显隐变化), 再重算内容尺寸
    JZScrollArea::resizeEvent(event);
    updateContentSize();
    update();
}
