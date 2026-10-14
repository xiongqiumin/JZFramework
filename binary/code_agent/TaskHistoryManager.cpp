#include "TaskHistoryManager.h"

#include <chrono>
#include <fstream>
#include <iterator>

#include "JZDebug.h"
#include "JZDir.h"
#include "JZFileInfo.h"

using namespace std;

// 说明(2026-10-14): 本文件原来直接 include "sqlite3.h" 手写 prepare/bind/step/finalize。
// 现在改用框架自己的 C++ 封装(JZSqlDatabase / JZSqlQuery, 见 JZDatabase/JZSqlDatabase.h):
//   · 使用方(本文件 + 编译它的各宿主)不再碰 sqlite3 的 C API ⇒ 动态库下也不用把几百个
//     sqlite3_* 导出(否则 code_agent_test / taskstore_test 这类会 LNK2019);
//   · prepare/finalize 由 JZSqlQuery 的 RAII 负责, 错误统一看 lastError()。

// 任务要跨进程/跨天显示"最近"顺序，时间戳必须是墙钟（纪元毫秒）。
// 不能用 JZPlatform::getTickCountMs()：那是 steady_clock，进程重启就归零。
static int64_t wallClockMs()
{
    using namespace std::chrono;
    return (int64_t)duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// ==================== HistoryEntry ====================
Json::Value HistoryEntry::toJson() const
{
    Json::Value root;
    root["id"] = id;
    root["title"] = title;
    root["firstMessage"] = firstMessage;
    root["createdAt"] = createdAt;
    root["messageCount"] = messageCount;
    root["workspace"] = workspace;
    return root;
}

void HistoryEntry::fromJson(const Json::Value &root)
{
    id = root.get("id", 0).asInt();
    title = root.get("title", "").asString();
    firstMessage = root.get("firstMessage", "").asString();
    createdAt = root.get("createdAt", 0).asInt64();
    messageCount = root.get("messageCount", 0).asInt();
    workspace = root.get("workspace", "").asString();
}

// ==================== TaskHistoryManager ====================
TaskHistoryManager::TaskHistoryManager()
{
}

TaskHistoryManager::~TaskHistoryManager()
{
    closeDb();
}

// ==================== SQLite ====================
string TaskHistoryManager::dbPath() const
{
    if (m_storageDir.empty())
        return string();
    return m_storageDir + "/tasks.db";
}

void TaskHistoryManager::closeDb()
{
    m_db.close();
}

bool TaskHistoryManager::createTables()
{
    if (!m_db.isOpen())
        return false;

    // 多语句一次建好（exec 支持分号分隔的多条语句）
    const char* sql =
        "CREATE TABLE IF NOT EXISTS tasks ("
        "  id            INTEGER PRIMARY KEY,"
        "  title         TEXT    NOT NULL DEFAULT '',"
        "  first_message TEXT    NOT NULL DEFAULT '',"
        "  created_at    INTEGER NOT NULL DEFAULT 0,"
        "  message_count INTEGER NOT NULL DEFAULT 0,"
        "  workspace     TEXT    NOT NULL DEFAULT '');"
        "CREATE INDEX IF NOT EXISTS idx_tasks_workspace ON tasks(workspace);"
        "CREATE TABLE IF NOT EXISTS conversations ("
        "  task_id INTEGER PRIMARY KEY,"
        "  json    TEXT    NOT NULL);";

    if (!m_db.exec(sql))
    {
        jzDebug() << "[TaskHistoryManager] create tables failed:" << m_db.lastError().c_str();
        return false;
    }
    return true;
}

bool TaskHistoryManager::openDb()
{
    closeDb();
    if (m_storageDir.empty())
        return false;

    JZDir dir;
    dir.mkpath(m_storageDir);

    string path = dbPath();
    if (!m_db.open(path))
    {
        // 打开失败不致命：任务功能降级为"仅内存"，界面照常可用
        jzDebug() << "[TaskHistoryManager] open db failed:" << path
                  << m_db.lastError().c_str();
        m_db.close();
        return false;
    }

    m_db.setBusyTimeout(3000);   // 多进程/被杀进程留下的锁，短暂等待而非直接失败
    return createTables();
}

bool TaskHistoryManager::loadFromDb()
{
    if (!m_db.isOpen())
        return false;

    JZSqlQuery q;
    if (!q.prepare(m_db,
            "SELECT id, title, first_message, created_at, message_count, workspace"
            " FROM tasks ORDER BY id"))
        return false;

    while (q.step())
    {
        HistoryEntry e;
        e.id = q.columnInt(0);
        e.title = q.columnText(1);
        e.firstMessage = q.columnText(2);
        e.createdAt = q.columnInt64(3);
        e.messageCount = q.columnInt(4);
        e.workspace = q.columnText(5);
        if (e.id > 0)
            m_all.push_back(e);
    }
    return true;
}

bool TaskHistoryManager::writeEntryRow(const HistoryEntry& e)
{
    if (!m_db.isOpen())
        return false;

    JZSqlQuery q;
    if (!q.prepare(m_db,
            "INSERT OR REPLACE INTO tasks"
            "(id, title, first_message, created_at, message_count, workspace)"
            " VALUES(?, ?, ?, ?, ?, ?)"))
        return false;

    q.bind(1, e.id);
    q.bind(2, e.title);
    q.bind(3, e.firstMessage);
    q.bind(4, e.createdAt);
    q.bind(5, e.messageCount);
    q.bind(6, e.workspace);

    // 写语句正常"跑完"落在 DONE 上 ⇒ isDone() 才算成功(与旧代码 step()==SQLITE_DONE 等价)
    q.step();
    return q.isDone();
}

void TaskHistoryManager::deleteEntryRow(int id)
{
    if (!m_db.isOpen())
        return;

    JZSqlQuery q;
    if (!q.prepare(m_db, "DELETE FROM tasks WHERE id = ?"))
        return;
    q.bind(1, id);
    q.step();
}

// 旧版本（index.json + task_<id>.json）一次性搬进库：
// 只在"库里一条任务都没有"且旧 index.json 存在时执行，旧文件保留不删。
void TaskHistoryManager::importLegacyFiles()
{
    if (!m_db.isOpen() || m_storageDir.empty())
        return;

    {
        JZSqlQuery cnt;
        if (!cnt.prepare(m_db, "SELECT COUNT(*) FROM tasks") || !cnt.step())
            return;
        if (cnt.columnInt64(0) > 0)
            return;      // 库里已有数据：不是首次打开，直接跳过
    }

    string idxPath = m_storageDir + "/index.json";
    if (!JZFileInfo::exists(idxPath))
        return;

    ifstream f(idxPath);
    if (!f.is_open())
        return;

    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(f, root) || !root.isMember("history"))
        return;
    f.close();

    const Json::Value& arr = root["history"];
    int imported = 0;
    for (int i = 0; i < (int)arr.size(); i++)
    {
        HistoryEntry e;
        e.fromJson(arr[i]);
        if (e.id <= 0)
            continue;
        writeEntryRow(e);
        imported++;

        // 对话正文：旧的 task_<id>.json 原样搬进 conversations 表
        string oldFile = m_storageDir + "/task_" + to_string(e.id) + ".json";
        if (!JZFileInfo::exists(oldFile))
            continue;

        ifstream cf(oldFile, ios::binary);
        if (!cf.is_open())
            continue;
        string text((istreambuf_iterator<char>(cf)), istreambuf_iterator<char>());
        if (text.empty())
            continue;

        JZSqlQuery ins;
        if (!ins.prepare(m_db, "INSERT OR REPLACE INTO conversations(task_id, json) VALUES(?, ?)"))
            continue;
        ins.bind(1, e.id);
        ins.bind(2, text);
        ins.step();
    }

    if (imported > 0)
        jzDebug() << "[TaskHistoryManager] imported" << imported
                  << "legacy tasks from index.json into tasks.db";
}

// ==================== 存储目录 / 工程 ====================
void TaskHistoryManager::setStorageDir(const std::string& dir)
{
    if (m_storageDir == dir)
        return;
    m_storageDir = dir;
    m_all.clear();
    m_visible.clear();
    m_currentId = 0;
    openDb();          // 打开库并建表；任务加载由 load() 负责
    bumpVersion();
}

void TaskHistoryManager::bumpVersion()
{
    m_version++;
}

// 按当前工程重建可见列表；当前任务若不属于本工程，就落到本工程最新一条
void TaskHistoryManager::rebuildVisible()
{
    m_visible.clear();
    for (const auto& e : m_all)
    {
        if (e.workspace == m_workspace)
            m_visible.push_back(e);
    }

    bool currentOk = false;
    for (const auto& e : m_visible)
    {
        if (e.id == m_currentId)
        {
            currentOk = true;
            break;
        }
    }
    if (!currentOk)
        m_currentId = m_visible.empty() ? 0 : m_visible.back().id;
}

void TaskHistoryManager::setWorkspace(const std::string& ws)
{
    if (m_workspace == ws)
        return;
    m_workspace = ws;
    rebuildVisible();
    bumpVersion();
}

void TaskHistoryManager::load()
{
    m_all.clear();

    if (!m_db.isOpen())
        openDb();

    if (m_db.isOpen())
    {
        importLegacyFiles();     // 首次打开：把旧的 index.json 任务搬进来
        loadFromDb();
    }

    rebuildVisible();

    // 当前工程还没有任务：建一条，界面上永远有"当前任务"可发消息
    if (!m_storageDir.empty() && !m_workspace.empty() && m_visible.empty())
        appendEntry("新任务");

    bumpVersion();
}

void TaskHistoryManager::save()
{
    // SQLite 版每次改动即时落盘（见 writeEntryRow），这个接口只为兼容保留。
}

int TaskHistoryManager::nextId() const
{
    int maxId = 0;
    for (const auto& e : m_all)
    {
        if (e.id > maxId)
            maxId = e.id;
    }
    return maxId + 1;
}

HistoryEntry* TaskHistoryManager::findEntry(int id)
{
    for (auto& e : m_all)
    {
        if (e.id == id)
            return &e;
    }
    return nullptr;
}

const HistoryEntry* TaskHistoryManager::findEntry(int id) const
{
    for (const auto& e : m_all)
    {
        if (e.id == id)
            return &e;
    }
    return nullptr;
}

// 追加一条属于当前工程的任务并切过去
HistoryEntry* TaskHistoryManager::appendEntry(const std::string& title)
{
    HistoryEntry entry;
    entry.id = nextId();
    entry.title = title;
    entry.createdAt = wallClockMs();
    entry.workspace = m_workspace;
    m_all.push_back(entry);
    m_currentId = entry.id;
    m_visible.push_back(entry);
    writeEntryRow(entry);
    bumpVersion();
    return &m_all.back();
}

int TaskHistoryManager::newTask()
{
    if (m_storageDir.empty() || m_workspace.empty())
        return 0;

    // 当前任务还没说过话（新建/清空后一直空着）：复用它，不堆一串空的"新任务"
    HistoryEntry* cur = findEntry(m_currentId);
    if (cur && cur->messageCount <= 0 && cur->firstMessage.empty())
    {
        bumpVersion();
        return m_currentId;
    }

    return appendEntry("新任务")->id;
}

bool TaskHistoryManager::switchTask(int id)
{
    if (id <= 0 || id == m_currentId)
        return false;
    if (!findEntry(id))
        return false;

    m_currentId = id;
    bumpVersion();
    return true;
}

bool TaskHistoryManager::deleteTask(int id)
{
    bool currentDeleted = false;
    bool found = false;
    for (auto it = m_all.begin(); it != m_all.end(); ++it)
    {
        if (it->id == id)
        {
            currentDeleted = (m_currentId == id);
            m_all.erase(it);
            found = true;
            break;
        }
    }
    if (!found)
        return false;

    deleteEntryRow(id);
    clearConversation(id);
    rebuildVisible();

    if (currentDeleted)
    {
        // 落到本工程剩余最新一条；一条都不剩就新建一条空任务
        if (m_visible.empty())
            appendEntry("新任务");
        else
            m_currentId = m_visible.back().id;
    }
    bumpVersion();
    return true;
}

void TaskHistoryManager::touchTask(const std::string& firstMessage)
{
    if (m_storageDir.empty() || m_workspace.empty() || firstMessage.empty())
        return;

    HistoryEntry* cur = findEntry(m_currentId);
    if (!cur)
    {
        cur = appendEntry(generateTitle(firstMessage));
        cur->firstMessage = firstMessage;
        cur->messageCount = 1;
    }
    else
    {
        if (cur->firstMessage.empty())
        {
            cur->title = generateTitle(firstMessage);
            cur->firstMessage = firstMessage;
        }
        cur->messageCount++;
    }
    commitEntryChange();
}

void TaskHistoryManager::addMessage()
{
    HistoryEntry* cur = findEntry(m_currentId);
    if (!cur)
        return;
    cur->messageCount++;
    commitEntryChange();
}

void TaskHistoryManager::resetMessageCount()
{
    HistoryEntry* cur = findEntry(m_currentId);
    if (cur)
        cur->messageCount = 0;
    commitEntryChange();
}

// 改完 m_all 里的条目后统一收尾：m_visible 是它的投影，必须一起刷新
void TaskHistoryManager::commitEntryChange()
{
    HistoryEntry* cur = findEntry(m_currentId);
    if (cur)
        writeEntryRow(*cur);
    rebuildVisible();
    bumpVersion();
}

// ==================== 对话正文 ====================
bool TaskHistoryManager::saveConversation(int taskId, const Json::Value& json)
{
    if (!m_db.isOpen() || taskId <= 0)
        return false;
    if (json.isNull())
        return clearConversation(taskId);

    JZSqlQuery q;
    if (!q.prepare(m_db, "INSERT OR REPLACE INTO conversations(task_id, json) VALUES(?, ?)"))
        return false;

    string text = json.toStyledString();
    q.bind(1, taskId);
    q.bind(2, text);
    q.step();
    return q.isDone();
}

Json::Value TaskHistoryManager::conversationJson(int taskId) const
{
    if (!m_db.isOpen() || taskId <= 0)
        return Json::Value();

    // 本函数是 const 成员: JZSqlQuery::prepare 收的是 const 库引用(见其声明), 所以直接查
    JZSqlQuery q;
    if (!q.prepare(m_db, "SELECT json FROM conversations WHERE task_id = ?"))
        return Json::Value();
    q.bind(1, taskId);
    if (!q.step())
        return Json::Value();

    string text = q.columnText(0);
    if (text.empty())
        return Json::Value();

    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(text, root))
        return Json::Value();
    return root;
}

bool TaskHistoryManager::clearConversation(int taskId)
{
    if (!m_db.isOpen() || taskId <= 0)
        return false;

    JZSqlQuery q;
    if (!q.prepare(m_db, "DELETE FROM conversations WHERE task_id = ?"))
        return false;
    q.bind(1, taskId);
    q.step();
    return q.isDone();
}

string TaskHistoryManager::generateTitle(const std::string &firstMessage)
{
    if (firstMessage.empty())
        return "空任务";

    // 截取第一条消息的前 50 个字符作为标题
    size_t maxLen = 50;
    if (firstMessage.size() > maxLen)
    {
        string sub = firstMessage.substr(0, maxLen);
        // 在最后一个空格处截断，避免截断单词
        size_t lastSpace = sub.find_last_of(" \t\n");
        if (lastSpace != string::npos && lastSpace > 10)
            sub = sub.substr(0, lastSpace);
        sub += "...";
        return sub;
    }
    return firstMessage;
}
