#ifndef JZ_MOUSE_EVENT_H_
#define JZ_MOUSE_EVENT_H_

#include "JZEvent.h"
#include "JZPoint.h"

// 鼠标事件: 坐标均为逻辑像素 (HiDPI 缩放已由显示设备换算)。
// globalPos 相对显示设备左上角, localPos 相对目标控件左上角。
class JZMouseEvent : public JZEvent
{
public:
    enum Button {
        NoButton = 0,
        LeftButton,
        RightButton,
        MiddleButton,
    };

    enum MouseType {
        Press,
        Release,
        Move,
        Wheel,
    };

    JZMouseEvent(MouseType type, Button button, const JZPoint& globalPos,
                 const JZPoint& localPos, int wheelDelta = 0);

    MouseType mouseType() const { return m_mouseType; }
    Button button() const { return m_button; }
    const JZPoint& globalPos() const { return m_globalPos; }
    const JZPoint& localPos() const { return m_localPos; }
    int wheelDelta() const { return m_wheelDelta; }   // 滚轮格数 (120 原始增量 / 120)

private:
    MouseType m_mouseType;
    Button m_button;
    JZPoint m_globalPos;
    JZPoint m_localPos;
    int m_wheelDelta = 0;
};

#endif // JZ_MOUSE_EVENT_H_
