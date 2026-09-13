#ifndef JZ_PAINTER_EVENT_H_
#define JZ_PAINTER_EVENT_H_

#include "JZEvent.h"
#include "JZRect.h"

class JZPainter;

class JZInvalidateEvent : public JZEvent
{
public:
    explicit JZInvalidateEvent();

    JZRect rect;
};

// Created by the render scheduler during the refresh traversal.
// 只携带**重绘区域** rect() (脏区 ∩ 控件全局 rect, 供虚拟列表/表格只画可见部分)。
//
// 画笔不在事件里 —— 与全框架约定一致: 每个控件在自己的 paintEvent 里自建
// `JZPainter p(this)` (裁剪的权威设定点在 painter 构造函数, 见 JZPainter.cpp),
// 事件过滤器里需要绘制时同样按被拦截的那个控件自建 painter。
class JZPainterEvent : public JZEvent
{
public:
    explicit JZPainterEvent();

    void setRect(const JZRect& rc) { m_rect = rc; }
    JZRect rect() const { return m_rect; }

private:
    JZRect m_rect;
};

#endif