#ifndef JZ_MOUSE_EVENT_H_
#define JZ_MOUSE_EVENT_H_

#include "JZEvent.h"
#include "JZKeyEvent.h"   // 复用 JZKeyEvent::Modifier (修饰键位组合, 与键盘事件同一套常量)
#include "JZPoint.h"

// 鼠标事件: 坐标均为逻辑像素 (HiDPI 缩放已由显示设备换算)。
// globalPos 为屏幕逻辑坐标 (与 Qt 的 QMouseEvent::globalPos 一致);
// localPos 为相对目标控件左上角的逻辑坐标 (命中分发时回填);
// modifiers 为构造瞬间的 Shift/Ctrl/Alt 状态 (等价 Qt QMouseEvent::modifiers,
//   Qt 也是"事件创建时快照", 拖动中途按下/松开修饰键不会改变已在途的事件)。
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
                 const JZPoint& localPos, int wheelDelta = 0,
                 uint32_t modifiers = JZKeyEvent::NoModifier);

    MouseType mouseType() const { return m_mouseType; }
    Button button() const { return m_button; }
    const JZPoint& globalPos() const { return m_globalPos; }
    const JZPoint& localPos() const { return m_localPos; }
    void setLocalPos(const JZPoint& p) { m_localPos = p; }   // 分发层命中测试后回填
    int wheelDelta() const { return m_wheelDelta; }   // 滚轮格数 (120 原始增量 / 120)

    // 修饰键 (JZKeyEvent::Modifier 位组合): 平台消息层在构造事件时填入。
    // 判断用 modifiers() 与位常量相与, 如 Ctrl+点击:
    //   (ev->modifiers() & JZKeyEvent::CtrlModifier) != 0
    uint32_t modifiers() const { return m_modifiers; }

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
    uint32_t m_modifiers = JZKeyEvent::NoModifier;
    bool m_programmatic = false;
};

#endif // JZ_MOUSE_EVENT_H_
