#include "TaskHistoryManager.h"

#include <chrono>
#include <fstream>

#include "JZDir.h"
#include "JZFile.h"
#include "JZFileInfo.h"

using namespace std;

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

void TaskHistoryManager::setStorageDir(const std::string& dir)
{
    if (m_storageDir == dir)
        return;
    m_storageDir = dir;
    m_all.clear();
    m_visible.clear();
    m_currentId = 0;
    m_nextId = 1;
    bumpVersion();
}

string TaskHistoryManager::indexPath() const
{
    if (m_storageDir.empty())
        return string();
    return m_storageDir + "/index.json";
}

string TaskHistoryManager::archivePath(int id) const
{
    if (m_storageDir.empty() || id <= 0)
        return string();
    return m_storageDir + "/task_" + to_string(id) + ".json";
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
    m_nextId = 1;

    string path = indexPath();
    if (!path.empty() && JZFileInfo::exists(path))
    {
        ifstream f(path);
        if (f.is_open())
        {
            Json::Value root;
            Json::Reader reader;
            if (reader.parse(f, root) && root.isMember("history"))
            {
                const Json::Value& arr = root["history"];
                for (int i = 0; i < (int)arr.size(); i++)
                {
                    HistoryEntry entry;
                    entry.fromJson(arr[i]);
                    if (entry.id > 0)
                        m_all.push_back(entry);
                }
            }
            m_nextId = root.isMember("nextTaskId") ? root["nextTaskId"].asInt() : 1;
        }
    }

    // 索引里的 nextTaskId 可能与已有 id 冲突（手改/旧数据），统一顶到最大 id 之后
    for (const auto& e : m_all)
    {
        if (e.id >= m_nextId)
            m_nextId = e.id + 1;
    }
    if (m_nextId <= 0)
        m_nextId = 1;

    rebuildVisible();

    // 当前工程还没有任务：建一条，界面上永远有"当前任务"可发消息
    if (!m_storageDir.empty() && !m_workspace.empty() && m_visible.empty())
        appendEntry("新任务");

    bumpVersion();
}

void TaskHistoryManager::save()
{
    string path = indexPath();
    if (path.empty())
        return;

    JZDir dir;
    dir.mkpath(m_storageDir);

    Json::Value root;
    Json::Value history(Json::arrayValue);
    for (const auto& entry : m_all)
        history.append(entry.toJson());
    root["history"] = history;
    root["nextTaskId"] = m_nextId;

    Json::StyledWriter writer;
    ofstream f(path);
    if (f.is_open())
        f << writer.write(root);
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
    entry.id = m_nextId++;
    entry.title = title;
    entry.createdAt = wallClockMs();
    entry.workspace = m_workspace;
    m_all.push_back(entry);
    m_currentId = entry.id;
    m_visible.push_back(entry);
    save();
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

    removeArchive(id);
    rebuildVisible();

    if (currentDeleted)
    {
        // 落到本工程剩余最新一条；一条都不剩就新建一条空任务
        if (m_visible.empty())
            appendEntry("新任务");
        else
            m_currentId = m_visible.back().id;
    }
    save();
    bumpVersion();
    return true;
}

void TaskHistoryManager::removeArchive(int id)
{
    string path = archivePath(id);
    if (!path.empty() && JZFileInfo::exists(path))
        JZFile::remove(path);
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
    rebuildVisible();
    save();
    bumpVersion();
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
