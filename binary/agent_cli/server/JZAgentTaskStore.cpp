#include "JZAgentTaskStore.h"

#include "JZSqlDatabase.h"
#include "JZDir.h"
#include "JZDateTime.h"
#include "JZDebug.h"

#include <utility>

namespace {

const char *kStateWorking = "TASK_STATE_WORKING";
const char *kStateSubmitted = "TASK_STATE_SUBMITTED";
const char *kStateInputRequired = "TASK_STATE_INPUT_REQUIRED";
const char *kStateAuthRequired = "TASK_STATE_AUTH_REQUIRED";
const char *kStateUnknown = "TASK_STATE_UNSPECIFIED";
const char *kStateCanceled = "TASK_STATE_CANCELED";

bool isNonTerminal(const std::string &state)
{
    return state == kStateWorking || state == kStateSubmitted ||
           state == kStateInputRequired || state == kStateAuthRequired ||
           state == kStateUnknown;
}

bool parseJsonText(const std::string &text, Json::Value &out)
{
    Json::Reader reader;
    return reader.parse(text, out) && !out.isNull();
}

std::string toJsonLine(const Json::Value &v)
{
    std::string s = Json::FastWriter().write(v);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
        s.pop_back();
    return s;
}

std::vector<std::string> stringsFromJson(const Json::Value &arr)
{
    std::vector<std::string> out;
    if (!arr.isArray())
        return out;
    for (Json::ArrayIndex i = 0; i < arr.size(); ++i)
    {
        const Json::Value &v = arr[i];
        out.push_back(v.isString() ? v.asString() : toJsonLine(v));
    }
    return out;
}

Json::Value jsonFromStoredText(const std::string &text)
{
    Json::Value v;
    if (text.empty() || !parseJsonText(text, v))
        return Json::Value();
    return v;
}

std::string storedJsonText(const Json::Value &v)
{
    if (!v.isObject() && !v.isArray())
        return std::string();
    return toJsonLine(v);
}

const char *kTaskDbSchema =
    "CREATE TABLE IF NOT EXISTS tasks ("
    " ord INTEGER PRIMARY KEY AUTOINCREMENT,"
    " task_id TEXT NOT NULL UNIQUE,"
    " agent_id TEXT, state TEXT, context_id TEXT, project TEXT, origin TEXT,"
    " parent_task_id TEXT, prompt TEXT, verify_cmd TEXT, error TEXT, last_event_type TEXT,"
    " rounds INTEGER DEFAULT 0, round INTEGER DEFAULT 0,"
    " start_ms INTEGER DEFAULT 0, end_ms INTEGER DEFAULT 0, status_ms INTEGER DEFAULT 0,"
    " event_seq INTEGER DEFAULT 0,"
    " usage_json TEXT, usage_turn_json TEXT, payload_json TEXT NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_tasks_status ON tasks(status_ms DESC);"
    "CREATE INDEX IF NOT EXISTS idx_tasks_agent_state ON tasks(agent_id, state);"
    "CREATE INDEX IF NOT EXISTS idx_tasks_context ON tasks(context_id);"
    "CREATE INDEX IF NOT EXISTS idx_tasks_origin_status ON tasks(origin, status_ms DESC);";

} // namespace

JZAgentTaskStore::JZAgentTaskStore(const std::string &dataDir)
{
    m_dataDir = dataDir;
    m_dbFile = dataDir + "/tasks.db";
}

JZAgentTaskStore::~JZAgentTaskStore()
{
    close();
}

bool JZAgentTaskStore::open(std::string *error)
{
    close();

    JZDir dataDir;
    dataDir.mkpath(m_dataDir);

    m_db = new JZSqlDatabase();
    if (!m_db->open(m_dbFile)) {
        if (error)
            *error = m_db->lastError();
        jzWarning() << "任务库打不开:" << m_db->lastError();
        delete m_db;
        m_db = nullptr;
        return false;
    }
    m_db->setBusyTimeout(3000);
    if (!m_db->exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;") ||
        !m_db->exec(kTaskDbSchema)) {
        if (error)
            *error = m_db->lastError();
        jzWarning() << "任务库初始化失败:" << m_db->lastError();
        m_db->close();
        delete m_db;
        m_db = nullptr;
        return false;
    }
    return true;
}

void JZAgentTaskStore::close()
{
    if (!m_db)
        return;
    m_db->close();
    delete m_db;
    m_db = nullptr;
}

bool JZAgentTaskStore::load(std::vector<JZAgentTaskRecord> &records, std::string *error) const
{
    records.clear();
    if (!m_db)
        return false;

    JZSqlQuery q;
    if (!q.prepare(*m_db,
                   "SELECT task_id, agent_id, state, context_id, project, origin, parent_task_id,"
                   " prompt, verify_cmd, error, last_event_type, rounds, round,"
                   " start_ms, end_ms, status_ms, event_seq, usage_json, usage_turn_json, payload_json"
                   " FROM tasks ORDER BY ord ASC")) {
        if (error)
            *error = q.lastError();
        jzWarning() << "任务库查询失败:" << q.lastError();
        return false;
    }

    const long long now = (long long)JZDateTime::currentMSecsSinceEpoch();
    while (q.step()) {
        JZAgentTaskRecord r;
        r.taskId = q.columnText(0);
        r.agentId = q.columnText(1);
        r.state = q.columnText(2);
        r.contextId = q.columnText(3);
        r.project = q.columnText(4);
        r.origin = q.columnText(5);
        r.parentTaskId = q.columnText(6);
        r.prompt = q.columnText(7);
        r.verifyCmd = q.columnText(8);
        r.error = q.columnText(9);
        r.lastEventType = q.columnText(10);
        r.rounds = q.columnInt(11);
        r.round = q.columnInt(12);
        r.startMs = q.columnInt64(13);
        r.endMs = q.columnInt64(14);
        r.statusMs = q.columnInt64(15);
        r.eventSeq = q.columnInt(16);
        r.usage = jsonFromStoredText(q.columnText(17));
        r.usageTurn = jsonFromStoredText(q.columnText(18));
        r.payloadJson = q.columnText(19);
        r.detailLoaded = false;

        // 上次进程没等到回包就退出了: 非终态一律按未完成取消处理。
        if (isNonTerminal(r.state)) {
            r.state = kStateCanceled;
            r.error.clear();
            r.endMs = now;
        }
        records.push_back(std::move(r));
    }
    return true;
}

void JZAgentTaskStore::ensureDetail(JZAgentTaskRecord &r) const
{
    if (r.detailLoaded)
        return;
    r.detailLoaded = true;
    if (r.payloadJson.empty())
        return;
    Json::Value j;
    if (!parseJsonText(r.payloadJson, j) || !j.isObject()) {
        r.payloadJson.clear();
        return;
    }
    Json::Value meta = j.get("metadata", Json::Value(Json::objectValue));
    r.events = stringsFromJson(meta.get("events", Json::Value(Json::arrayValue)));
    r.artifacts = stringsFromJson(j.get("artifacts", Json::Value(Json::arrayValue)));
    r.history = stringsFromJson(j.get("history", Json::Value(Json::arrayValue)));
    if (!r.usage.isObject())
        r.usage = meta.get("usage", Json::Value());
    if (!r.usageTurn.isObject())
        r.usageTurn = meta.get("usage_turn", Json::Value());
    r.eventSeq = meta.get("eventSeq", (int)r.events.size()).asInt();
    r.payloadJson.clear();
}

void JZAgentTaskStore::upsert(JZAgentTaskRecord &r)
{
    if (!m_db)
        return;

    std::string payload = r.payloadJson;
    if (payload.empty()) {
        if (!m_payloadBuilder) {
            jzWarning() << "任务库缺少 payload 构造器, 写入跳过:" << r.taskId;
            return;
        }
        payload = m_payloadBuilder(r);
        r.payloadJson = payload;
    }

    const std::string usage = storedJsonText(r.usage);
    const std::string usageTurn = storedJsonText(r.usageTurn);

    auto bindColumns = [&](JZSqlQuery &q, int base) {
        q.bind(base++, r.agentId);
        q.bind(base++, r.state);
        q.bind(base++, r.contextId);
        q.bind(base++, r.project);
        q.bind(base++, r.origin);
        q.bind(base++, r.parentTaskId);
        q.bind(base++, r.prompt);
        q.bind(base++, r.verifyCmd);
        q.bind(base++, r.error);
        q.bind(base++, r.lastEventType);
        q.bind(base++, r.rounds);
        q.bind(base++, r.round);
        q.bind(base++, (int64_t)r.startMs);
        q.bind(base++, (int64_t)r.endMs);
        q.bind(base++, (int64_t)r.statusMs);
        q.bind(base++, r.eventSeq);
        q.bind(base++, usage);
        q.bind(base++, usageTurn);
        q.bind(base++, payload);
    };

    JZSqlQuery update;
    if (update.prepare(*m_db,
                       "UPDATE tasks SET agent_id=?,state=?,context_id=?,project=?,origin=?,"
                       "parent_task_id=?,prompt=?,verify_cmd=?,error=?,last_event_type=?,"
                       "rounds=?,round=?,start_ms=?,end_ms=?,status_ms=?,event_seq=?,"
                       "usage_json=?,usage_turn_json=?,payload_json=? WHERE task_id=?")) {
        bindColumns(update, 1);
        update.bind(20, r.taskId);
        update.step();
        if (m_db->changes() > 0)
            return;
    }

    JZSqlQuery insert;
    if (!insert.prepare(*m_db,
                        "INSERT INTO tasks(task_id,agent_id,state,context_id,project,origin,"
                        "parent_task_id,prompt,verify_cmd,error,last_event_type,"
                        "rounds,round,start_ms,end_ms,status_ms,event_seq,"
                        "usage_json,usage_turn_json,payload_json)"
                        " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)")) {
        jzWarning() << "任务库写入准备失败:" << insert.lastError();
        return;
    }
    insert.bind(1, r.taskId);
    bindColumns(insert, 2);
    insert.step();
    if (!insert.isDone() && !m_db->lastError().empty())
        jzWarning() << "任务库写入失败:" << m_db->lastError();
}

void JZAgentTaskStore::prune(const std::set<std::string> &liveIds)
{
    if (!m_db)
        return;

    JZSqlQuery q;
    if (!q.prepare(*m_db, "SELECT task_id FROM tasks"))
        return;
    std::vector<std::string> stale;
    while (q.step()) {
        const std::string id = q.columnText(0);
        if (liveIds.find(id) == liveIds.end())
            stale.push_back(id);
    }
    q.finalize();
    if (stale.empty())
        return;

    JZSqlQuery del;
    if (!del.prepare(*m_db, "DELETE FROM tasks WHERE task_id=?"))
        return;
    m_db->exec("BEGIN IMMEDIATE;");
    for (const std::string &id : stale) {
        del.reset();
        del.clearBindings();
        del.bind(1, id);
        del.step();
    }
    del.finalize();
    m_db->exec("COMMIT;");
}
