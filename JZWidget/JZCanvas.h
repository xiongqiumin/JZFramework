#ifndef JZ_CANVAS_H_
#define JZ_CANVAS_H_

#include "JZWidget.h"
#include <functional>

class JZPainter;

// 通用绘制表面: 通过回调把 JZPainter 交给调用方(JS 或 C++)自由绘制,
// 并重发鼠标按下事件。自身不绘制任何内容(背景由 paint 回调负责铺),
// 故默认设为不透明以参与窗口覆盖剔除。
//
// 典型用法(配合 JS 桥 JZJSWidgetModule): 普通 JZWidget 子类, 事件走 onPaint / onMousePress
// 属性(由 JSWidgetClass 转发), 回调里用 e.painter() 取到的 JZPainter 自由作画。
class JZCanvas : public JZWidget
{
    JZ_OBJECT_DECLARE(JZCanvas)
public:
    using PaintCallback = std::function<void(JZPainter&)>;
    using MouseCallback = std::function<void(int x, int y, int button)>;

    JZCanvas(JZWidget* parent = nullptr);
    ~JZCanvas();

    void setPaintCallback(PaintCallback cb) { m_paintCb = std::move(cb); }
    void setMouseCallback(MouseCallback cb) { m_mouseCb = std::move(cb); }

protected:
    void paintEvent(JZPainterEvent* event) override;
    void mousePressEvent(JZMouseEvent* event) override;

    PaintCallback m_paintCb;
    MouseCallback m_mouseCb;
};

#endif // !JZ_CANVAS_H_
