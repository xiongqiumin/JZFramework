#include "NavPanel.h"
#include "JZLayout.h"
#include "JZStd.h"

using namespace std;

NavPanel::NavPanel(JZWidget* parent)
    : JZWidget(parent)
    , sigNewProject(this), sigOpenProject(this), sigNewTask(this)
    , sigExport(this), sigSettings(this)
    , sigTaskActivated(this), sigDeleteTask(this)
{
    setMinimumWidth(200);

    JZVBoxLayout* v = new JZVBoxLayout(6, 8);
    setLayout(v);

    // 搜索框
    m_search = new JZLineEdit(this);
    m_search->setPlaceholderText("搜索");
    m_search->setFixedHeight(32);
    v->addWidget(m_search, 0);

    // 固定功能入口
    m_fixedList = new JZListWidget(this);
    m_fixedList->setItemHeight(34);
    m_fixedList->addItem("新建工程");
    m_fixedList->addItem("打开工程");
    m_fixedList->addItem("新建任务");
    m_fixedList->addItem("导出工程");
    m_fixedList->addItem("设置");
    v->addWidget(m_fixedList, 0);

    // 任务区: TaskPanel(有工程) / 空态提示(无工程)
    m_taskArea = new JZStackedWidget(this);

    m_taskPanel = new TaskPanel(m_taskArea);
    m_taskArea->addWidget(m_taskPanel);

    m_emptyArea = new JZWidget(m_taskArea);
    {
        JZVBoxLayout* ev = new JZVBoxLayout(0, 0);
        m_emptyArea->setLayout(ev);
        ev->addStretch(1);
        JZLabel* empty = new JZLabel("打开工程后\n这里会显示任务", m_emptyArea);
        empty->setAlign(1);
        ev->addWidget(empty, 0);
        ev->addStretch(1);
    }
    m_taskArea->addWidget(m_emptyArea);
    v->addWidget(m_taskArea, 1);

    // 内部 TaskPanel 的信号转发给 NavPanel
    connect(&m_taskPanel->sigNewTask, this, [this]() { sigNewTask.emit(); });
    connect(&m_taskPanel->sigTaskActivated, this, [this](int id) { sigTaskActivated.emit(id); });
    connect(&m_taskPanel->sigDeleteTask, this, [this](int id) { sigDeleteTask.emit(id); });

    // 固定入口点击 -> 对应信号
    connect(&m_fixedList->sigActivated, this, &NavPanel::onFixedActivated);
    // 搜索 -> 过滤任务
    connect(&m_search->sigTextChanged, this, &NavPanel::onSearchTextChanged);
}

void NavPanel::setTasks(const vector<HistoryEntry>& tasks, int currentId)
{
    m_allTasks = tasks;
    m_currentId = currentId;
    applyFilter();
}

void NavPanel::setProjectOpened(bool opened)
{
    if (m_taskArea)
        m_taskArea->setCurrentIndex(opened ? 0 : 1);
}

void NavPanel::applyFilter()
{
    if (!m_taskPanel)
        return;
    if (m_searchText.empty())
    {
        m_taskPanel->setTasks(m_allTasks, m_currentId);
        return;
    }
    string ql = str_toLower(m_searchText);
    vector<HistoryEntry> filtered;
    for (const auto& t : m_allTasks)
    {
        string title = t.title.empty() ? string("新任务") : t.title;
        if (str_toLower(title).find(ql) != string::npos)
            filtered.push_back(t);
    }
    m_taskPanel->setTasks(filtered, m_currentId);
}

void NavPanel::onFixedActivated(int index)
{
    switch (index)
    {
    case 0: sigNewProject.emit(); break;
    case 1: sigOpenProject.emit(); break;
    case 2: sigNewTask.emit(); break;
    case 3: sigExport.emit(); break;
    case 4: sigSettings.emit(); break;
    }
    m_fixedList->setCurrentIndex(-1);   // 清除高亮: 导航项不是"当前页"
}

void NavPanel::onSearchTextChanged(const std::string& text)
{
    m_searchText = text;
    applyFilter();
}

JZ_OBJECT_IMPL(NavPanel, JZWidget)
