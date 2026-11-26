#ifndef JZ_AGENT_TASK_STORE_H_
#define JZ_AGENT_TASK_STORE_H_

#include <functional>
#include <set>
#include <string>
#include <vector>

#include "JZJson.h"

class JZSqlDatabase;

// 任务历史记录: server 是 owner。
// 只有一套模型: A2A v1.0 Task。内部字段名也用状态词本身, 不再存 running/success 那套。
struct JZAgentTaskRecord
{
    std::string taskId, agentId, prompt, verifyCmd;
    std::string state;
    std::string error;
    std::string contextId;
    std::string project;
    std::string origin;
    std::string parentTaskId;
    std::string lastEventType;
    int rounds = 0, round = 0;
    long long startMs = 0, endMs = 0, statusMs = 0;
    int eventSeq = 0;
    Json::Value usage;
    Json::Value usageTurn;
    std::vector<std::string> events;
    std::string openKind;
    std::string openText;
    long long openAt = 0;
    std::vector<std::string> artifacts;
    std::vector<std::string> history;

    // SQLite: 启动时只载摘要 + payloadJson; 详情按需从 payloadJson 解析一次。
    mutable bool detailLoaded = true;
    std::string payloadJson;
};

class JZAgentTaskStore
{
public:
    using PayloadBuilder = std::function<std::string(JZAgentTaskRecord &)>;

    explicit JZAgentTaskStore(const std::string &dataDir);
    ~JZAgentTaskStore();

    bool open(std::string *error = nullptr);
    void close();
    bool isOpen() const { return m_db != nullptr; }
    const std::string &dbFile() const { return m_dbFile; }

    void setPayloadBuilder(PayloadBuilder builder) { m_payloadBuilder = std::move(builder); }

    bool load(std::vector<JZAgentTaskRecord> &records, std::string *error = nullptr) const;
    void ensureDetail(JZAgentTaskRecord &r) const;
    void upsert(JZAgentTaskRecord &r);
    void prune(const std::set<std::string> &liveIds);

private:
    JZSqlDatabase *m_db = nullptr;
    std::string m_dataDir;
    std::string m_dbFile;
    PayloadBuilder m_payloadBuilder;
};

#endif // JZ_AGENT_TASK_STORE_H_
