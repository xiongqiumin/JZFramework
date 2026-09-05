#ifndef JZ_MOUSE_EVENT_H_
#define JZ_MOUSE_EVENT_H_

#include "JZEvent.h"
#include "JZPoint.h"

// 鼠标事件: 坐标均为逻辑像素 (HiDPI 缩放已由显示设备换算)。
// globalPos 为屏幕逻辑坐标 (与 Qt 的 QMouseEvent::globalPos 一致);
// localPos 为相对目标控件左上角的逻辑坐标 (命中分发时回填)。
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
    void setLocalPos(const JZPoint& p) { m_localPos = p; }   // 分发层命中测试后回填
    int wheelDelta() const { return m_wheelDelta; }   // 滚轮格数 (120 原始增量 / 120)

    // 程序化 (框架/AI/测试) 合成注入的输入: 分发时不受全屏蒙版输入屏蔽限制
    // (蒙版只挡真实用户, 不挡 AI 自己的点击)。默认 false = 真实用户输入。
    bool isProgrammatic() const { return m_programmatic; }
    void setProgrammatic(bool on) { m_programmatic = on; }

private:
    MouseType m_mouseType;
    Button m_button;
    JZPoint m_globalPos;
    JZPoint m_localPos;
    int m_wheelDelta = 0;
    bool m_programmatic = false;
};

#endif // JZ_MOUSE_EVENT_H_
