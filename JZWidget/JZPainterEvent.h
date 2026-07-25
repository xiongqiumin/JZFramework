#ifndef JZ_PAINTER_EVENT_H_
#define JZ_PAINTER_EVENT_H_

#include "JZEvent.h"

// 在刷新遍历中随 paintEvent 一起下发, 子类通过 painter() 拿到当前图层的绘制入口。
class JZPainterEvent : public JZEvent
{
public:
    explicit JZPainterEvent();


private:

};

#endif
