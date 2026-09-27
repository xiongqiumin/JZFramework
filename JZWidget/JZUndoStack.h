#ifndef JZ_UNDO_STACK_H_
#define JZ_UNDO_STACK_H_
#include "JZGlobal.h"
#include <string>
#include <vector>

// ==================== 撤销/重做框架 (仿 Qt QUndoCommand / QUndoStack) ====================
// 与 Qt 的语义对齐:
//   - JZUndoCommand 表示一步可撤销操作: redo() 执行, undo() 回退; 通过
//     id()/canMergeWith()/mergeWith() 支持把相邻命令"合并"成一步 (Qt 用来自动
//     合并连续输入), 也支持带子命令的父命令 (宏: undo/redo 自动遍历孩子)。
//   - JZUndoStack 管理命令序列与当前"已执行到哪一步"的下标 index()。
//     push() 接管命令所有权并立即执行 redo(); 若栈顶命令与新命令可合并,
//     则合并进栈顶而不再新增一步。undo()/redo() 移动 index 并调用对应命令。
//   - 可选的撤销深度上限 undoLimit (0 = 不限, 默认), 以及"干净点"
//     setClean()/isClean() (语义: 内容是否处于保存过的状态)。
//
// 说明: 命令默认不允许合并 (canMergeWith 返回 false); 需要自动合并的场合
// (例如文本编辑按时间窗合并连续输入) 由具体命令覆写 id()/canMergeWith()/
// mergeWith()。撤销后 push 新命令会丢弃全部 redo 分支 (与 Qt 一致)。

class JZFRAMEWORK_API JZUndoCommand
{
public:
    explicit JZUndoCommand(const std::string& text = std::string(),
                           JZUndoCommand* parent = nullptr);
    virtual ~JZUndoCommand();

    void setText(const std::string& text);
    std::string text() const;

    // 由栈调用, 子类覆写实现具体效果。带子命令的父命令 (宏) 默认实现会
    // 自动递归孩子 (undo 逆序, redo 顺序), 无需覆写。
    virtual void undo();
    virtual void redo();

    // ---- 合并协议 ----
    // 只有 id() != -1 且与栈顶命令相同才可能合并; 真正是否合并由
    // canMergeWith() 决定。合并时新命令先 redo, 再通过 mergeWith()
    // 把其效果写回栈顶命令, 新命令本身被丢弃。
    virtual int id() const { return -1; }
    virtual bool canMergeWith(const JZUndoCommand* other) const;
    virtual void mergeWith(const JZUndoCommand* other) { (void)other; }

    // 子命令访问 (宏/组合命令)
    int childCount() const;
    JZUndoCommand* child(int index);
    const JZUndoCommand* child(int index) const;

protected:
    JZUndoCommand* parent() const { return m_parent; }
    void setParent(JZUndoCommand* parent);
    void appendChild(JZUndoCommand* command);

private:
    friend class JZUndoStack;   // 宏实现需要把命令挂到宏父命令上
    std::string m_text;
    JZUndoCommand* m_parent = nullptr;
    std::vector<JZUndoCommand*> m_children;
};

class JZFRAMEWORK_API JZUndoStack
{
public:
    JZUndoStack();
    ~JZUndoStack();

    void clear();
    void push(JZUndoCommand* command);      // 接管所有权, 并立即执行 redo()
    void undo();
    void redo();
    void setIndex(int index);               // 撤销/重做到指定位置

    int  index() const { return m_index; }
    int  count() const { return static_cast<int>(m_commands.size()); }
    bool canUndo() const { return m_index > 0; }
    bool canRedo() const { return m_index < static_cast<int>(m_commands.size()); }

    JZUndoCommand* command(int index);
    const JZUndoCommand* command(int index) const;

    // 撤销深度上限: 超过部分从栈底丢弃; 0 = 不限制 (默认)
    void setUndoLimit(int limit);
    int  undoLimit() const { return m_undoLimit; }

    bool isClean() const { return m_cleanIndex >= 0 && m_cleanIndex == m_index; }
    int  cleanIndex() const { return m_cleanIndex; }
    void setClean() { m_cleanIndex = m_index; }

    // 宏: beginMacro/endMacro 之间 push 的所有命令合并为一步 (Qt 语义)。
    // 空宏 (期间没有 push) 不会产生撤销步。
    void beginMacro(const std::string& text);
    void endMacro();

    std::string undoText() const;           // 下一次 undo 命令的描述 (可空)
    std::string redoText() const;           // 下一次 redo 命令的描述 (可空)

private:
    void removeCommandsFrom(int from);      // 丢弃 [from, size) 的命令 (redo 分支)
    void trimToLimit();                     // 按 undoLimit 从栈底裁剪

    std::vector<JZUndoCommand*> m_commands;
    int m_index = 0;
    int m_undoLimit = 0;
    int m_cleanIndex = 0;                   // 干净点时 index; -1 = 永不干净
    int m_macroDepth = 0;
    JZUndoCommand* m_macro = nullptr;       // 当前宏命令 (begin 创建, end 入栈)
};

#endif // JZ_UNDO_STACK_H_
