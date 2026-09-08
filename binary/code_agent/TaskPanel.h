#ifndef JZ_CODE_AGENT_TASKPANEL_H_
#define JZ_CODE_AGENT_TASKPANEL_H_

#include "JZWidget.h"
#include "JZListWidget.h"
#include "JZPushButton.h"

#include <vector>

#include "TaskHistoryManager.h"

// 任务面板（Codex 风格侧栏）：顶部标题 + [新建任务]，中间任务列表，底部 [删除]。
// 列表每项两行：标题 + "N 条消息 / 未开始"；最新的任务排在最上面，当前任务高亮。
// 只负责展示与发信号，真正的对话归档/切换由 JZCodeAgent 的任务管理完成。
class TaskPanel : public JZWidget
{
    JZ_OBJECT_DECLARE(TaskPanel)
public:
    explicit TaskPanel(JZWidget* parent = nullptr);

    // 用任务列表重建列表；currentId 对应的行设为高亮选中
    void setTasks(const std::vector<HistoryEntry>& tasks, int currentId);
    int  currentTaskId() const;      // 当前选中行的任务 id（-1 = 没有选中）

    JZSigVoid sigNewTask;
    JZSigInt  sigTaskActivated;      // 点击任务项 -> 任务 id
    JZSigInt  sigDeleteTask;         // 删除选中的任务 -> 任务 id

private:
    void onNewTask();
    void onDeleteTask();
    void onItemActivated(int index);

    JZListWidget* m_list = nullptr;
    JZPushButton* m_newBtn = nullptr;
    JZPushButton* m_delBtn = nullptr;
    std::vector<int> m_ids;          // 行号 -> 任务 id
    int m_currentId = -1;            // 当前任务 id（自绘时用来画高亮）
};

#endif // !JZ_CODE_AGENT_TASKPANEL_H_
