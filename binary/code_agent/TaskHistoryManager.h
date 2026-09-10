#ifndef JZ_CODE_AGENT_TASKHISTORYMANAGER_H_
#define JZ_CODE_AGENT_TASKHISTORYMANAGER_H_

#include <string>
#include <vector>

#include "json/json.h"

struct sqlite3;   // 前向声明：不把 SQLite 头文件暴露给任务管理的使用方

// 一条历史任务（= 一段独立对话）的元数据。
// 对话正文不在里面：它以 JSON 存进任务库的 conversations 表，这里只记 id / 标题 / 消息数等。
struct HistoryEntry
{
    int    id = 0;                    // 唯一递增 ID（= tasks 表主键）
    std::string title;                // 自动生成的标题（第一条用户消息摘要）
    std::string firstMessage;         // 第一条用户消息原文
    int64_t createdAt = 0;            // 创建时间（墙钟纪元毫秒，用于跨会话排序/显示）
    int    messageCount = 0;          // 对话消息数（用户+助手）
    // 该任务所属工程（工作区）的绝对路径：任务数据不放在工程里，但要记住它属于哪个工程
    std::string workspace;

    Json::Value toJson() const;
    void fromJson(const Json::Value &root);
};

// 历史任务管理：只管"任务"这一层（条目 + 对话正文的存取），不碰对话内容本身。
//
// 存储（**应用目录**，不落进用户工程）：
//   <应用目录>/tasks/tasks.db     SQLite 库
// 表结构：
//   tasks(id INTEGER PRIMARY KEY, title, first_message, created_at, message_count, workspace)
//   conversations(task_id INTEGER PRIMARY KEY, json TEXT)   -- 对话正文(JZAgent 序列化的 JSON)
//
// 列表对外只暴露"当前工程"的任务（tasks()），切换工程即换一批可见任务。
//
// 历史版本存的是 index.json + task_<id>.json 两个文件；首次打开时若库里一条任务都没有
// 而旧 index.json 还在，会自动导入一次（旧文件保留不删，供回滚/备份）。
class TaskHistoryManager
{
public:
    TaskHistoryManager();
    ~TaskHistoryManager();

    // 存储根目录（应用目录 + "/tasks"）；启动时设一次即可（会打开/建库）
    void setStorageDir(const std::string& dir);
    const std::string& storageDir() const { return m_storageDir; }

    // 当前工程：新建任务时写进条目，同时决定 tasks() 过滤出哪一批
    void setWorkspace(const std::string& ws);
    const std::string& workspace() const { return m_workspace; }

    void load();                       // 载入任务；当前工程还没有任务就建第一条
    // 兼容保留：SQLite 版每次改动即时落盘，这里不需要做任何事。
    void save();

    const std::vector<HistoryEntry>& tasks() const { return m_visible; }  // 当前工程的任务
    int  currentTaskId() const { return m_currentId; }
    // 变更计数：界面据此判断要不要重建列表（避免每帧刷新）
    int  taskVersion() const { return m_version; }

    int  newTask();                    // 新建任务并切过去（当前任务还空着就复用它）
    bool switchTask(int id);           // 切换当前任务；id 非法 / 就是当前任务返回 false
    bool deleteTask(int id);           // 删除任务及其对话；删掉当前任务时落到剩余最新一条

    HistoryEntry* findEntry(int id);
    const HistoryEntry* findEntry(int id) const;

    // 首条用户消息落地：给任务定标题并计一条消息
    void touchTask(const std::string& firstMessage);
    void addMessage();                 // 一轮结束再计一条（助手回复）
    void resetMessageCount();          // 清空对话后计数归零

    // ---- 对话正文（tasks.db 的 conversations 表） ----
    std::string dbPath() const;                                  // <存储目录>/tasks.db
    bool saveConversation(int taskId, const Json::Value& json);  // json 为 null 等价于 clearConversation
    Json::Value conversationJson(int taskId) const;              // 没有则返回 null
    bool clearConversation(int taskId);

    // 自动生成标题（截取第一条消息前 N 个字符）
    static std::string generateTitle(const std::string &firstMessage);

private:
    void bumpVersion();                // 变更计数 +1
    void rebuildVisible();             // 按当前工程重建可见列表，并校正当前任务
    void commitEntryChange();          // 条目被改动后的统一收尾（落盘 + 刷新投影 + 版本号）
    HistoryEntry* appendEntry(const std::string& title);
    int  nextId() const;               // 内存里最大 id + 1

    // ---- SQLite ----
    bool openDb();                     // 打开（并建目录/建表）；失败降级为内存态
    void closeDb();
    bool createTables();
    bool loadFromDb();
    bool writeEntryRow(const HistoryEntry& e);   // INSERT OR REPLACE 单条
    void deleteEntryRow(int id);
    void importLegacyFiles();          // 旧 index.json / task_<id>.json 一次性导入

    std::string m_storageDir;            // 应用级存储目录
    std::string m_workspace;             // 当前工程
    std::vector<HistoryEntry> m_all;     // 全部任务（按创建顺序）
    std::vector<HistoryEntry> m_visible; // 当前工程的任务（对外展示）
    int m_currentId = 0;                 // 当前任务 id（0 = 无）
    int m_version = 0;
    sqlite3* m_db = nullptr;             // 任务库连接（打开失败时为 nullptr，功能降级为仅内存）
};

#endif // !JZ_CODE_AGENT_TASKHISTORYMANAGER_H_
