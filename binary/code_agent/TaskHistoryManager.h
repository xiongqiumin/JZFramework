#ifndef JZ_CODE_AGENT_TASKHISTORYMANAGER_H_
#define JZ_CODE_AGENT_TASKHISTORYMANAGER_H_

#include <string>
#include <vector>

#include "json/json.h"

// 一条历史任务（= 一段独立对话）的元数据。
// 对话正文不在里面：它由 JZCodeAgent 存成独立存档文件，这里只记 id / 标题 / 消息数等。
struct HistoryEntry
{
    int    id = 0;                    // 唯一递增 ID
    std::string title;                // 自动生成的标题（第一条用户消息摘要）
    std::string firstMessage;         // 第一条用户消息原文
    int64_t createdAt = 0;            // 创建时间（墙钟纪元毫秒，用于跨会话排序/显示）
    int    messageCount = 0;          // 对话消息数（用户+助手）
    // 该任务所属工程（工作区）的绝对路径：任务数据不放在工程里，但要记住它属于哪个工程
    std::string workspace;

    Json::Value toJson() const;
    void fromJson(const Json::Value &root);
};

// 历史任务管理：只管"任务"这一层（索引 + 存档文件的位置/增删），不碰对话内容本身。
// 对话的存取由 JZCodeAgent 按 archivePath() 给出的路径完成。
//
// 存储布局（**应用目录**，不落进用户工程）：
//   <应用目录>/tasks/index.json       任务索引（全部工程的任务都在这，跨工程）
//   <应用目录>/tasks/task_<id>.json   该任务的对话存档
// 列表对外只暴露"当前工程"的任务（tasks()），切换工程即换一批可见任务。
class TaskHistoryManager
{
public:
    TaskHistoryManager();

    // 存储根目录（应用目录 + "/tasks"）；启动时设一次即可
    void setStorageDir(const std::string& dir);
    const std::string& storageDir() const { return m_storageDir; }

    // 当前工程：新建任务时写进条目，同时决定 tasks() 过滤出哪一批
    void setWorkspace(const std::string& ws);
    const std::string& workspace() const { return m_workspace; }

    void load();                       // 载入索引；当前工程还没有任务就建第一条
    void save();                       // 写回索引

    const std::vector<HistoryEntry>& tasks() const { return m_visible; }  // 当前工程的任务
    int  currentTaskId() const { return m_currentId; }
    // 变更计数：界面据此判断要不要重建列表（避免每帧刷新）
    int  taskVersion() const { return m_version; }

    int  newTask();                    // 新建任务并切过去（当前任务还空着就复用它）
    bool switchTask(int id);           // 切换当前任务；id 非法 / 就是当前任务返回 false
    bool deleteTask(int id);           // 删除任务及其存档文件；删掉当前任务时落到剩余最新一条

    HistoryEntry* findEntry(int id);
    const HistoryEntry* findEntry(int id) const;

    // 首条用户消息落地：给任务定标题并计一条消息
    void touchTask(const std::string& firstMessage);
    void addMessage();                 // 一轮结束再计一条（助手回复）
    void resetMessageCount();          // 清空对话后计数归零

    // ---- 存档文件 ----
    std::string indexPath() const;           // <存储目录>/index.json
    std::string archivePath(int id) const;   // <存储目录>/task_<id>.json
    void removeArchive(int id);              // 删除任务的对话存档

    // 自动生成标题（截取第一条消息前 N 个字符）
    static std::string generateTitle(const std::string &firstMessage);

private:
    void bumpVersion();                // 变更计数 +1
    void rebuildVisible();             // 按当前工程重建可见列表，并校正当前任务
    void commitEntryChange();          // 条目被改动后的统一收尾（刷新投影 + 落盘 + 版本号）
    HistoryEntry* appendEntry(const std::string& title);

    std::string m_storageDir;            // 应用级存储目录
    std::string m_workspace;             // 当前工程
    std::vector<HistoryEntry> m_all;     // 全部任务（按创建顺序）
    std::vector<HistoryEntry> m_visible; // 当前工程的任务（对外展示）
    int m_currentId = 0;                 // 当前任务 id（0 = 无）
    int m_nextId = 1;                    // 下一个任务 id
    int m_version = 0;
};

#endif // !JZ_CODE_AGENT_TASKHISTORYMANAGER_H_
