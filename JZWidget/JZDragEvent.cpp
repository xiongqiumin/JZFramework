#include "JZDragEvent.h"

JZDropEvent::JZDropEvent(int type, const JZPoint& pos, const std::shared_ptr<JZMimeData>& mimeData,
                         JZDropActions possibleActions, JZDropAction proposedAction,
                         uint32_t modifiers)
    : JZEvent(type)
{
    m_pos = pos;
    m_mimeData = mimeData;
    m_possibleActions = possibleActions;
    m_proposedAction = proposedAction;
    m_modifiers = modifiers;
    // Qt 语义: 拖放事件默认是 "不接"。必须显式 accept / acceptProposedAction 才成为
    // 拖放目标 —— 否则随便一个容器都会吞掉整次拖放, 拖方也收不到"可以放"的反馈。
    ignore();
}
