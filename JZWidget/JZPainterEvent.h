#ifndef JZ_PAINTER_EVENT_H_
#define JZ_PAINTER_EVENT_H_

#include "JZEvent.h"
#include "JZRect.h"

class JZPainter;

// 在刷新遍历中由 JZDisplayDevice::refresh() 统一创建 JZPainter 并注入。
// 子类通过 painter() 拿到当前图层的绘制入口, 通过 rect() 拿到本次需要重绘的绘制区域
// (等价脏区/裁剪区, 供虚拟列表、表格等只绘制可见部分)。
class JZPainterEvent : public JZEvent
{
public:
    explicit JZPainterEvent();

    void setPainter(JZPainter* painter) { m_painter = painter; }
    JZPainter* painter() const { return m_painter; }

    void setRect(const JZRect& rect) { m_rect = rect; }
    JZRect rect() const { return m_rect; }

private:
    JZPainter* m_painter = nullptr;
    JZRect m_rect;
};

#endif
