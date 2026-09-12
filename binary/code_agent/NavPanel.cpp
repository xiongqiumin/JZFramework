#include "NavPanel.h"
#include "JZLayout.h"
#include "JZStd.h"

using namespace std;

NavPanel::NavPanel(JZWidget* parent)
    : JZWidget(parent)
    , sigNewTask(this)
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

void NavPanel::onSearchTextChanged(const std::string& text)
{
    m_searchText = text;
    applyFilter();
}

JZ_OBJECT_IMPL(NavPanel, JZWidget)
