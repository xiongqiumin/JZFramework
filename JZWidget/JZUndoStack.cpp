#include "JZUndoStack.h"

#include <algorithm>

// ==================== JZUndoCommand ====================

JZUndoCommand::JZUndoCommand(const std::string& text, JZUndoCommand* parent)
    : m_text(text)
{
    if (parent)
        parent->appendChild(this);
}

JZUndoCommand::~JZUndoCommand()
{
    for (JZUndoCommand* child : m_children)
        delete child;
    m_children.clear();
}

void JZUndoCommand::setText(const std::string& text)
{
    m_text = text;
}

std::string JZUndoCommand::text() const
{
    return m_text;
}

// 父命令 (宏): undo 逆序 / redo 顺序遍历孩子; 叶命令默认无操作
void JZUndoCommand::undo()
{
    if (m_children.empty())
        return;
    for (int i = static_cast<int>(m_children.size()) - 1; i >= 0; --i)
        m_children[static_cast<size_t>(i)]->undo();
}

void JZUndoCommand::redo()
{
    for (JZUndoCommand* child : m_children)
        child->redo();
}

bool JZUndoCommand::canMergeWith(const JZUndoCommand*) const
{
    // Qt 默认行为: 即使 id 相同也不自动合并, 由具体命令决定
    return false;
}

int JZUndoCommand::childCount() const
{
    return static_cast<int>(m_children.size());
}

JZUndoCommand* JZUndoCommand::child(int index)
{
    return const_cast<JZUndoCommand*>(
        static_cast<const JZUndoCommand*>(this)->child(index));
}

const JZUndoCommand* JZUndoCommand::child(int index) const
{
    if (index < 0 || index >= static_cast<int>(m_children.size()))
        return nullptr;
    return m_children[static_cast<size_t>(index)];
}

void JZUndoCommand::setParent(JZUndoCommand* parent)
{
    m_parent = parent;
}

void JZUndoCommand::appendChild(JZUndoCommand* command)
{
    if (!command || command->m_parent == this)
        return;
    if (command->m_parent) {
        // 从旧父命令脱离 (仅移除指针, 不删除对象)
        std::vector<JZUndoCommand*>& old =
            command->m_parent->m_children;
        old.erase(std::remove(old.begin(), old.end(), command), old.end());
    }
    command->m_parent = this;
    m_children.push_back(command);
}

// ==================== JZUndoStack ====================

JZUndoStack::JZUndoStack()
{
}

JZUndoStack::~JZUndoStack()
{
    delete m_macro;
    m_macro = nullptr;
    clear();
}

void JZUndoStack::clear()
{
    for (JZUndoCommand* cmd : m_commands)
        delete cmd;
    m_commands.clear();
    m_index = 0;
    m_cleanIndex = 0;
}

void JZUndoStack::push(JZUndoCommand* command)
{
    if (!command)
        return;

    // 新命令使全部 redo 分支作废
    if (m_index < static_cast<int>(m_commands.size()))
        removeCommandsFrom(m_index);

    // 宏内: 作为宏的子命令, 立即执行但不单独入栈
    if (m_macro) {
        command->redo();
        m_macro->appendChild(command);
        return;
    }

    // 合并: 与栈顶(最后一个已执行命令)合并
    if (m_index > 0) {
        JZUndoCommand* top = m_commands[static_cast<size_t>(m_index - 1)];
        if (command->id() != -1 && top->id() == command->id()
            && top->canMergeWith(command)) {
            command->redo();            // Qt 语义: push 总是先执行
            top->mergeWith(command);
            delete command;
            return;
        }
    }

    command->redo();
    m_commands.push_back(command);
    ++m_index;
    trimToLimit();
}

void JZUndoStack::undo()
{
    if (!canUndo())
        return;
    m_commands[static_cast<size_t>(m_index - 1)]->undo();
    --m_index;
}

void JZUndoStack::redo()
{
    if (!canRedo())
        return;
    m_commands[static_cast<size_t>(m_index)]->redo();
    ++m_index;
}

void JZUndoStack::setIndex(int index)
{
    index = std::max(0, std::min(index, count()));
    while (m_index < index)
        redo();
    while (m_index > index)
        undo();
}

JZUndoCommand* JZUndoStack::command(int index)
{
    return const_cast<JZUndoCommand*>(
        static_cast<const JZUndoStack*>(this)->command(index));
}

const JZUndoCommand* JZUndoStack::command(int index) const
{
    if (index < 0 || index >= static_cast<int>(m_commands.size()))
        return nullptr;
    return m_commands[static_cast<size_t>(index)];
}

void JZUndoStack::setUndoLimit(int limit)
{
    m_undoLimit = std::max(0, limit);
    if (m_undoLimit > 0)
        trimToLimit();
}

std::string JZUndoStack::undoText() const
{
    if (!canUndo())
        return std::string();
    return m_commands[static_cast<size_t>(m_index - 1)]->text();
}

std::string JZUndoStack::redoText() const
{
    if (!canRedo())
        return std::string();
    return m_commands[static_cast<size_t>(m_index)]->text();
}

void JZUndoStack::beginMacro(const std::string& text)
{
    if (m_macroDepth == 0)
        m_macro = new JZUndoCommand(text);
    ++m_macroDepth;
}

void JZUndoStack::endMacro()
{
    if (m_macroDepth <= 0)
        return;
    --m_macroDepth;
    if (m_macroDepth > 0)
        return;                     // 嵌套宏: 等最外层结束

    JZUndoCommand* macro = m_macro;
    m_macro = nullptr;
    if (!macro)
        return;
    if (macro->childCount() == 0) { // 空宏不占一步
        delete macro;
        return;
    }

    if (m_index < static_cast<int>(m_commands.size()))
        removeCommandsFrom(m_index);
    // 子命令在 push 时已各自 redo 过, 宏整体直接入栈 (不重复执行)
    m_commands.push_back(macro);
    ++m_index;
    trimToLimit();
}

void JZUndoStack::removeCommandsFrom(int from)
{
    const int size = static_cast<int>(m_commands.size());
    if (from < 0 || from >= size)
        return;
    for (int i = from; i < size; ++i)
        delete m_commands[static_cast<size_t>(i)];
    m_commands.resize(static_cast<size_t>(from));
    // "干净点"落在被丢弃的命令上 -> 再也回不到该状态
    if (m_cleanIndex >= from)
        m_cleanIndex = -1;
}

void JZUndoStack::trimToLimit()
{
    if (m_undoLimit <= 0)
        return;
    while (static_cast<int>(m_commands.size()) > m_undoLimit) {
        if (m_cleanIndex <= 0)
            m_cleanIndex = -1;      // 栈底命令被删 (干净点命令本身被删)
        else
            --m_cleanIndex;         // 列表整体前移一格
        delete m_commands.front();
        m_commands.erase(m_commands.begin());
        --m_index;
    }
}
