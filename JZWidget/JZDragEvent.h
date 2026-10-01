#ifndef JZ_DRAG_EVENT_H_
#define JZ_DRAG_EVENT_H_
#include <inttypes.h>
#include <memory>

#include "JZEvent.h"
#include "JZKeyEvent.h"   // 复用 JZKeyEvent::Modifier (与鼠标/键盘同一套修饰键常量)
#include "JZPoint.h"
#include "JZMimeData.h"

// 拖放动作 (等价 Qt::DropAction, 并合并了 Qt 里 paperwork 式的 flags/enum 两份:
// 单个动作就是一个值, "支持哪些动作"用位或组合成的 JZDropActions)。
enum JZDropAction : uint32_t {
    JZDropActionNone  = 0x0,   // 拒绝放下 (setDropAction 用它表示 "不能放这里")
    JZDropActionCopy  = 0x1,   // 复制: 原始数据留在源处
    JZDropActionMove  = 0x2,   // 移动: 源应当在接受后删除自己的数据
    JZDropActionLink  = 0x4,   // 建立链接/快捷方式
    JZDropActionIgnore = 0x8,  // 忽略这次拖放 (内部用途: 未表态时的初始值)
};
typedef uint32_t JZDropActions;   // 位组合 (JZDropActionCopy | JZDropActionMove ...)

// ============================================================================
// 拖放事件 (等价 Qt 的 QDragEnterEvent / QDragMoveEvent / QDragLeaveEvent / QDropEvent)
// ============================================================================
// 层次与 Qt 一致:
//     JZDropEvent         放下 (Drop)        —— 也是下面两个的基类 (Qt 同款继承)
//       JZDragMoveEvent   拖动到本控件上方 (DragMove)
//         JZDragEnterEvent 刚进入本控件 (DragEnter)
//     JZDragLeaveEvent    离开本控件 (DragLeave, 独立一支)
//
// 语义要点 (见 JZWindow 的 OLE drop target 实现):
//   * 只有 setAcceptDrops(true) 的控件会收到; 沿父链逐级投递, 未被 accept 的继续上抛
//     (与 Qt 一致 —— 于是"容器整体接受拖放"不必给每个子控件都开 acceptDrops);
//   * 事件默认 **ignore**: 想收就得显式 accept() (或 acceptProposedAction());
//     拖方据此显示"禁止"光标并让 Drop 不被派发;
//   * pos() 是相对接收控件的局部逻辑坐标 (同 JZMouseEvent::localPos, 分发时回填);
//   * mimeData() 借用的那份在整个拖放期间有效 (dragEnter -> dragMove -> drop 同一份)。

class JZFRAMEWORK_API JZDropEvent : public JZEvent
{
public:
    JZDropEvent(int type, const JZPoint& pos, const std::shared_ptr<JZMimeData>& mimeData,
                JZDropActions possibleActions, JZDropAction proposedAction,
                uint32_t modifiers = JZKeyEvent::NoModifier);

    const JZPoint& pos() const { return m_pos; }
    void setPos(const JZPoint& pos) { m_pos = pos; }   // 分发层按目标控件回填

    JZMimeData* mimeData() { return m_mimeData.get(); }
    const JZMimeData* mimeData() const { return m_mimeData.get(); }

    // 拖动方能支持哪些动作 (由 IDataObject 的 effect 与修饰键共同决定)
    JZDropActions possibleActions() const { return m_possibleActions; }
    bool supportsAction(JZDropAction action) const { return (m_possibleActions & action) != 0; }

    // 系统建议的动作 (修饰键推导: Ctrl=Copy / Shift=Move / Alt=Link)
    JZDropAction proposedAction() const { return m_proposedAction; }
    // 接收方最终选择的动作 (默认 JZDropActionIgnore = 还没选)
    JZDropAction dropAction() const { return m_dropAction; }
    void setDropAction(JZDropAction action) { m_dropAction = action; }

    // 常用写法: 接受系统建议的动作 (= Qt 的 acceptProposedAction())
    void acceptProposedAction()
    {
        setDropAction(m_proposedAction);
        accept();
    }

    uint32_t modifiers() const { return m_modifiers; }

protected:
    JZPoint                     m_pos;
    std::shared_ptr<JZMimeData> m_mimeData;
    JZDropActions               m_possibleActions = JZDropActionNone;
    JZDropAction                m_proposedAction = JZDropActionIgnore;
    JZDropAction                m_dropAction = JZDropActionIgnore;
    uint32_t                    m_modifiers = JZKeyEvent::NoModifier;
};

// 拖动经过本控件 (位置在变, 数据不变): 同样要 accept 才表示"这里可以放"。
class JZFRAMEWORK_API JZDragMoveEvent : public JZDropEvent
{
public:
    JZDragMoveEvent(const JZPoint& pos, const std::shared_ptr<JZMimeData>& mimeData,
                    JZDropActions possibleActions, JZDropAction proposedAction,
                    uint32_t modifiers = JZKeyEvent::NoModifier)
        : JZDropEvent(JZEvent_dragMove, pos, mimeData, possibleActions, proposedAction, modifiers)
    {
    }

protected:
    // 派生类 (dragEnter) 走这条: 自己给事件类型
    JZDragMoveEvent(int type, const JZPoint& pos, const std::shared_ptr<JZMimeData>& mimeData,
                    JZDropActions possibleActions, JZDropAction proposedAction,
                    uint32_t modifiers = JZKeyEvent::NoModifier)
        : JZDropEvent(type, pos, mimeData, possibleActions, proposedAction, modifiers)
    {
    }
};

// 刚拖进本控件: accept 之后本控件成为本次拖放的目标
// (之后的 dragMove / drop 都发给它, 直到拖离)。
class JZFRAMEWORK_API JZDragEnterEvent : public JZDragMoveEvent
{
public:
    JZDragEnterEvent(const JZPoint& pos, const std::shared_ptr<JZMimeData>& mimeData,
                     JZDropActions possibleActions, JZDropAction proposedAction,
                     uint32_t modifiers = JZKeyEvent::NoModifier)
        : JZDragMoveEvent(JZEvent_dragEnter, pos, mimeData, possibleActions,
                          proposedAction, modifiers)
    {
    }
};

// 拖离本控件 (鼠标移出 / 按 Esc 取消 / 拖放会话结束): 只做状态复原, 不带数据。
class JZFRAMEWORK_API JZDragLeaveEvent : public JZEvent
{
public:
    JZDragLeaveEvent() : JZEvent(JZEvent_dragLeave) {}
};

#endif // !JZ_DRAG_EVENT_H_
