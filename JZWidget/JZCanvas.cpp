#include "JZCanvas.h"

#include "JZPainter.h"
#include "JZMouseEvent.h"

JZ_OBJECT_IMPL(JZCanvas, JZWidget)

JZCanvas::JZCanvas(JZWidget* parent)
    : JZWidget(parent)
{
    setOpaque(255);   // 自身负责铺满背景, 参与窗口覆盖剔除
}

JZCanvas::~JZCanvas()
{
}

void JZCanvas::paintEvent(JZPainterEvent* event)
{
    // 渲染调度器已为每个控件创建好 painter 并注入 event (坐标 = 控件本地坐标)。
    JZPainter* p = event->painter();
    if (p && m_paintCb)
        m_paintCb(*p);
}

void JZCanvas::mousePressEvent(JZMouseEvent* event)
{
    if (m_mouseCb)
        m_mouseCb(event->localPos().x(), event->localPos().y(), (int)event->button());
}
