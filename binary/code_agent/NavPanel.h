#ifndef JZ_CODE_AGENT_NAVPANEL_H_
#define JZ_CODE_AGENT_NAVPANEL_H_

#include "JZWidget.h"
#include "JZLineEdit.h"
#include "JZStackedWidget.h"
#include "JZLabel.h"
#include "TaskPanel.h"
#include "TaskHistoryManager.h"

// 左侧常驻导航栏: 顶部搜索 + 任务列表。
// 始终显示: 未打开工程时也常驻, 此时任务区显示"打开工程后..."提示。
// 工程相关入口(新建工程/打开工程/导出工程/设置)统一放在主窗口的"文件"菜单里,
// 面板内不再重复; 内部复用 TaskPanel 渲染任务列表, 搜索框按标题过滤任务。
class NavPanel : public JZWidget
{
    JZ_OBJECT_DECLARE(NavPanel)
public:
    explicit NavPanel(JZWidget* parent = nullptr);

    // 刷新任务列表; currentId 高亮
    void setTasks(const std::vector<HistoryEntry>& tasks, int currentId);
    // 切换任务区显示: true=TaskPanel, false=空态提示
    void setProjectOpened(bool opened);

    JZSigVoid   sigNewTask;        // 内部 TaskPanel 转发: 新建任务
    JZSigInt    sigTaskActivated;  // 内部 TaskPanel 转发: 切换任务
    JZSigInt    sigDeleteTask;     // 内部 TaskPanel 转发: 删除任务

private:
    void applyFilter();                        // 按 m_searchText 过滤并刷新 m_taskPanel
    void onSearchTextChanged(const std::string& text);

    JZLineEdit*      m_search = nullptr;
    JZStackedWidget* m_taskArea = nullptr;     // [0] TaskPanel [1] 空态
    TaskPanel*       m_taskPanel = nullptr;
    JZWidget*        m_emptyArea = nullptr;    // 空态: 居中提示

    std::vector<HistoryEntry> m_allTasks;
    int m_currentId = -1;
    std::string m_searchText;
};

#endif // !JZ_CODE_AGENT_NAVPANEL_H_
