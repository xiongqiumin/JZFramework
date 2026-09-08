#include "TaskPanel.h"

#include "JZLabel.h"
#include "JZLayout.h"
#include "JZFontMetrics.h"
#include "JZPainter.h"
#include "JZVariant.h"

#include <ctime>
#include <string>

using namespace std;

// 相对时间：刚刚 / N 分钟前 / 今天 HH:MM / 昨天 HH:MM / MM-DD HH:MM / YYYY-MM-DD
// （Codex 侧栏也是"最近会话"用相对时间描述）
static string formatTimeAgo(int64_t ms)
{
    if (ms <= 0)
        return string();

    time_t nowT = time(nullptr);
    time_t t = (time_t)(ms / 1000);

    tm now{}, cur{};
#ifdef _WIN32
    localtime_s(&now, &nowT);
    localtime_s(&cur, &t);
#else
    localtime_r(&nowT, &now);
    localtime_r(&t, &cur);
#endif

    double diff = difftime(nowT, t);
    if (diff < 0)
        diff = 0;

    char buf[32];
    if (diff < 60)
        return "刚刚";
    if (diff < 3600)
        return to_string((int)(diff / 60)) + " 分钟前";

    auto isSameDay = [](const tm& a, const tm& b) {
        return a.tm_year == b.tm_year && a.tm_yday == b.tm_yday;
    };

    if (isSameDay(now, cur))
    {
        strftime(buf, sizeof(buf), "今天 %H:%M", &cur);
        return string(buf);
    }

    // 昨天：往前推一天比较
    time_t yT = nowT - 24 * 3600;
    tm y{};
#ifdef _WIN32
    localtime_s(&y, &yT);
#else
    localtime_r(&yT, &y);
#endif
    if (isSameDay(y, cur))
    {
        strftime(buf, sizeof(buf), "昨天 %H:%M", &cur);
        return string(buf);
    }

    if (now.tm_year == cur.tm_year)
    {
        strftime(buf, sizeof(buf), "%m-%d %H:%M", &cur);
        return string(buf);
    }

    strftime(buf, sizeof(buf), "%Y-%m-%d", &cur);
    return string(buf);
}

// 列表项第二行：没说过话显示"未开始"，否则显示相对时间 + 消息条数
static string taskSubText(const HistoryEntry& e)
{
    if (e.messageCount <= 0)
        return "未开始";

    string t = formatTimeAgo(e.createdAt);
    if (t.empty())
        return to_string(e.messageCount) + " 条消息";
    return t + " · " + to_string(e.messageCount) + " 条消息";
}

// 按像素宽度截断（UTF-8 安全：回退时跳过续字节），放不下就结尾加 "..."
static string elideText(const string& text, const JZFont& font, int maxW)
{
    if (maxW <= 0)
        return string();
    if (JZFontMetrics::textWidth(text, font) <= maxW)
        return text;

    string out = text;
    while (!out.empty() && JZFontMetrics::textWidth(out + "...", font) > maxW)
    {
        out.pop_back();
        while (!out.empty() && ((unsigned char)out.back() & 0xC0) == 0x80)
            out.pop_back();
    }
    return out + "...";
}

// ==================== TaskPanel ====================
TaskPanel::TaskPanel(JZWidget* parent)
    : JZWidget(parent)
    , sigNewTask(this)
    , sigTaskActivated(this)
    , sigDeleteTask(this)
{
    JZVBoxLayout* v = new JZVBoxLayout(6, 6);
    setLayout(v);

    JZLabel* title = new JZLabel("任务", this);
    title->setFixedHeight(24);
    v->addWidget(title, 0);

    // 列表自绘两行：标题 + 消息条数；item.text 用 '\n' 分隔两行，data 存任务 id。
    m_list = new JZListWidget(this);
    m_list->setItemHeight(44);
    m_list->setItemPaint([this](JZPainter& p, const JZRect& rect, JZListWidgetItem* item)
    {
        bool sel = (m_currentId > 0 && item->data.toInt() == m_currentId);

        string text = item->text;
        string title = text, sub;
        size_t nl = text.find('\n');
        if (nl != string::npos)
        {
            title = text.substr(0, nl);
            sub = text.substr(nl + 1);
        }

        p.fillRect(rect.x() + 2, rect.y() + 2, rect.width() - 4, rect.height() - 4,
                   sel ? JZColor(0x2b, 0x57, 0x9a) : JZColor(0xf0, 0xf0, 0xf0));

        int x = rect.x() + 10;
        int w = rect.width() - 20;

        JZFont f1(13), f2(11);
        p.setFont(f1);
        p.setPen(JZPen(sel ? JZColor::White : JZColor::Black, 1));
        p.drawText(x, rect.y() + JZFontMetrics::centeredTextTop(f1, 24),
                   elideText(title, f1, w).c_str());

        p.setFont(f2);
        p.setPen(JZPen(sel ? JZColor(0xd8, 0xe4, 0xf2) : JZColor(0x80, 0x80, 0x80), 1));
        p.drawText(x, rect.y() + 24 + JZFontMetrics::centeredTextTop(f2, 20),
                   elideText(sub, f2, w).c_str());
    });
    v->addWidget(m_list, 1);

    JZWidget* btnRow = new JZWidget(this);
    JZHBoxLayout* bh = new JZHBoxLayout(0, 6);
    m_newBtn = new JZPushButton(btnRow, 0, 30, "新建任务");
    m_delBtn = new JZPushButton(btnRow, 0, 30, "删除");
    bh->addWidget(m_newBtn, 1);
    bh->addWidget(m_delBtn, 1);
    btnRow->setLayout(bh);
    v->addWidget(btnRow, 0);

    connect(&m_newBtn->sigClicked, this, &TaskPanel::onNewTask);
    connect(&m_delBtn->sigClicked, this, &TaskPanel::onDeleteTask);
    connect(&m_list->sigActivated, this, &TaskPanel::onItemActivated);
}

void TaskPanel::setTasks(const vector<HistoryEntry>& tasks, int currentId)
{
    m_currentId = currentId;
    m_ids.clear();
    m_list->clear();

    int curRow = -1;
    for (auto it = tasks.rbegin(); it != tasks.rend(); ++it)   // 最新的任务排最上面
    {
        JZListWidgetItem item;
        item.text = it->title.empty() ? string("新任务") : it->title;
        item.text += "\n" + taskSubText(*it);
        item.data = JZVariant(it->id);
        m_list->addItem(item);
        m_ids.push_back(it->id);
        if (it->id == currentId)
            curRow = (int)m_ids.size() - 1;
    }
    m_list->setCurrentIndex(curRow);
}

int TaskPanel::currentTaskId() const
{
    int idx = m_list->currentIndex();
    if (idx < 0 || idx >= (int)m_ids.size())
        return -1;
    return m_ids[idx];
}

void TaskPanel::onNewTask()
{
    sigNewTask.emit();
}

void TaskPanel::onDeleteTask()
{
    int id = currentTaskId();
    if (id > 0)
        sigDeleteTask.emit(id);
}

void TaskPanel::onItemActivated(int index)
{
    if (index < 0 || index >= (int)m_ids.size())
        return;
    m_currentId = m_ids[index];   // 立即高亮，不等下一轮刷新
    sigTaskActivated.emit(m_ids[index]);
}

JZ_OBJECT_IMPL(TaskPanel, JZWidget)
