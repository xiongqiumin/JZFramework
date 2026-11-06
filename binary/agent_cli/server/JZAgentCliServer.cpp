#include "JZAgentCliServer.h"

#include "JZFrpServer.h"         // FRP 服务端(frps, 可选)
#include "JZNet/JZNetPacket.h"   // JZNetPackInit(frps 依赖)
#include "JZNet/JZNetServer.h"   // 控制面(hub): cli 出站连进来的 JZNetServer
#include "JZFrpPacket.h"         // JZFrpPacketInit
#include "JZAgentHubPacket.h"    // 控制面协议(cli <-> server)
#include "JZHttpServer.h"        // 后台网页 / 管理 API
#include "JZHttpStaticFiles.h"
#include "JZProcess.h"            // 拉起并监管 agent_cli 子进程
#include "JZPlatform.h"          // applicationDirPath
#include "JZFileInfo.h"          // exists
#include "JZMutex.h"             // JZMutex / JZMutexLocker(日志跨线程读写保护)
#include "JZDateTime.h"          // currentMSecsSinceEpoch
#include "JZTimer.h"             // 任务超时(到点回 504 并请求 cli 取消)
#include "JZDebug.h"
#include "JZJson.h"              // Json::Value / FastWriter

#include <deque>
#include <sstream>

using namespace std;

// ===========================================================================
// 单个被管理 agent 的运行实例
// ===========================================================================
struct JZCliServerAgentInstance
{
    JZCliServerAgentConfig cfg;
    JZProcess *proc = nullptr;
    JZProcess::ProcessState state = JZProcess::NotRunning;
    int exitCode = 0;
    JZProcess::ExitStatus exitStatus = JZProcess::NormalExit;
    int64_t startTime = 0;
    int64_t stopTime = 0;

    mutable JZMutex mutex;          // 保护 logs / state / exitCode(读线程写, HTTP 线程读)
    std::deque<std::string> logs;   // 日志环形缓冲(已按行拆分)
    std::string recvBuf;            // 未完成的半行(仅读线程访问, 无需锁)
    size_t logMax = 3000;
};

// ---------------------------------------------------------------------------
// 路由处理器(把 HTTP 请求转交给 JZAgentCliServer 的对应方法)
// ---------------------------------------------------------------------------
class DashboardHandler : public JZHttpHandler
{
public:
    explicit DashboardHandler(JZAgentCliServer *s) : m_s(s) {}
    void handle(JZHttpServerClient *c, const JZHttpRequest &r) override { m_s->onDashboard(c, r); }
private:
    JZAgentCliServer *m_s;
};

class ApiStatusHandler : public JZHttpHandler
{
public:
    explicit ApiStatusHandler(JZAgentCliServer *s) : m_s(s) {}
    void handle(JZHttpServerClient *c, const JZHttpRequest &r) override { m_s->onApiStatus(c, r); }
private:
    JZAgentCliServer *m_s;
};

class ApiAgentsHandler : public JZHttpHandler
{
public:
    explicit ApiAgentsHandler(JZAgentCliServer *s) : m_s(s) {}
    void handle(JZHttpServerClient *c, const JZHttpRequest &r) override { m_s->onApiAgents(c, r); }
private:
    JZAgentCliServer *m_s;
};

class ApiAgentLogsHandler : public JZHttpHandler
{
public:
    explicit ApiAgentLogsHandler(JZAgentCliServer *s) : m_s(s) {}
    void handle(JZHttpServerClient *c, const JZHttpRequest &r) override { m_s->onApiAgentLogs(c, r); }
private:
    JZAgentCliServer *m_s;
};

class ApiAgentActionHandler : public JZHttpHandler
{
public:
    ApiAgentActionHandler(JZAgentCliServer *s, const string &a) : m_s(s), m_action(a) {}
    void handle(JZHttpServerClient *c, const JZHttpRequest &r) override
    {
        m_s->onApiAgentAction(c, r, m_action);
    }
private:
    JZAgentCliServer *m_s;
    string m_action;   // start / stop / restart
};

// 控制面: 提交任务(POST /api/tasks) / 取消任务(POST /api/tasks/cancel)
class ApiTasksHandler : public JZHttpHandler
{
public:
    explicit ApiTasksHandler(JZAgentCliServer *s) : m_s(s) {}
    void handle(JZHttpServerClient *c, const JZHttpRequest &r) override { m_s->onApiTasks(c, r); }
private:
    JZAgentCliServer *m_s;
};

class ApiTaskCancelHandler : public JZHttpHandler
{
public:
    explicit ApiTaskCancelHandler(JZAgentCliServer *s) : m_s(s) {}
    void handle(JZHttpServerClient *c, const JZHttpRequest &r) override { m_s->onApiTaskCancel(c, r); }
private:
    JZAgentCliServer *m_s;
};

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------
static void respondJson(JZHttpServerClient *client, const JZHttpRequest &req, int code, const Json::Value &v)
{
    JZHttpRespone resp;
    resp.setStatusCode(code);
    resp.setJson(Json::FastWriter().write(v));
    client->response(resp, req);
}

static void respondText(JZHttpServerClient *client, const JZHttpRequest &req, int code,
                        const string &text, const string &ctype = "text/plain; charset=utf-8")
{
    JZHttpRespone resp;
    resp.setStatusCode(code);
    resp.setContentType(ctype);
    resp.setBody(text);
    client->response(resp, req);
}

// 任务回包是**异步**的(等 cli 跑完), 那时请求对象早没了 —— 只能用 buffer 版本。
static void respondJsonAsync(JZHttpServerClient *client, int code, const Json::Value &v)
{
    if (!client)
        return;
    JZHttpRespone resp;
    resp.setStatusCode(code);
    resp.setJson(Json::FastWriter().write(v));
    client->response(resp.toByteArray());
}

static bool parseJsonText(const string &text, Json::Value &out)
{
    Json::Reader reader;
    return reader.parse(text, out) && !out.isNull();
}

static const char *stateString(JZProcess::ProcessState s)
{
    switch (s)
    {
        case JZProcess::Running:  return "running";
        case JZProcess::Starting: return "starting";
        default:                  return "stopped";
    }
}

static Json::Value agentToJson(JZCliServerAgentInstance *inst)
{
    Json::Value j;
    j["name"] = inst->cfg.name;
    j["enabled"] = inst->cfg.enabled;
    j["cmd"] = inst->cfg.cmd;
    j["args"] = Json::arrayValue;
    for (const auto &a : inst->cfg.args)
        j["args"].append(a);
    j["running"] = (inst->state == JZProcess::Running);
    j["state"] = stateString(inst->state);
    j["exitCode"] = inst->exitCode;
    j["startTime"] = inst->startTime;
    j["stopTime"] = inst->stopTime;
    {
        JZMutexLocker lock(&inst->mutex);
        j["logLines"] = (int)inst->logs.size();
    }
    return j;
}

// ===========================================================================
// JZAgentCliServer
// ===========================================================================
JZAgentCliServer::JZAgentCliServer(JZObject *parent)
    : JZObject(parent)
{
}

JZAgentCliServer::~JZAgentCliServer()
{
    for (auto &kv : m_agents)
    {
        if (kv.second->proc && kv.second->state == JZProcess::Running)
            kv.second->proc->killProcess();
        delete kv.second;
    }
    m_agents.clear();
}

bool JZAgentCliServer::init(const JZCliServerConfig &config, std::string &error)
{
    (void)error;
    m_config = config;

    // 为每个 agent 配置建实例(进程随 server 一起存活), 并连好信号
    for (const auto &ac : m_config.agents)
    {
        auto *inst = new JZCliServerAgentInstance();
        inst->cfg = ac;
        inst->proc = new JZProcess(this);
        inst->proc->setProcessChannelMode(JZProcess::MergedChannels);   // stdout+stderr 合并
        if (!ac.dir.empty())
            inst->proc->setWorkingDirectory(ac.dir);

        // 收集日志(读线程触发); 跨线程写 logs, appendLog 内部加锁
        connect(&inst->proc->sigReadyReadStandardOutput,
                [this, inst]()
                {
                    JZByteArray b = inst->proc->readAllStandardOutput();
                    if (b.size() > 0)
                        appendLog(inst, str_fromUtf8(b));
                });
        // 启动成功
        connect(&inst->proc->sigStarted,
                [inst]()
                {
                    JZMutexLocker lock(&inst->mutex);
                    inst->state = JZProcess::Running;
                });
        // 结束: 把残留半行作为最后一行提交, 再更新状态
        connect(&inst->proc->sigFinished,
                [this, inst](int code, JZProcess::ExitStatus st)
                {
                    if (!inst->recvBuf.empty())
                    {
                        string tail = inst->recvBuf;
                        inst->recvBuf.clear();
                        appendLog(inst, tail + "\n");
                    }
                    JZMutexLocker lock(&inst->mutex);
                    inst->state = JZProcess::NotRunning;
                    inst->exitCode = code;
                    inst->exitStatus = st;
                    inst->stopTime = JZDateTime::currentMSecsSinceEpoch();
                });
        // 起不来(程序不存在等)
        connect(&inst->proc->sigErrorOccurred,
                [this, inst](JZProcess::ProcessError)
                {
                    appendLog(inst, "[error] 进程启动/运行失败\n");   // 内部加锁
                    JZMutexLocker lock(&inst->mutex);
                    inst->state = JZProcess::NotRunning;
                    inst->stopTime = JZDateTime::currentMSecsSinceEpoch();
                });

        m_agents[ac.name] = inst;
    }
    return true;
}

void JZAgentCliServer::start()
{
    m_startTime = JZDateTime::currentMSecsSinceEpoch();
    startFrp();    // 可选 frps
    startHub();    // 控制面(JZNet): 等 agent_cli 出站连进来
    startHttp();   // 后台网页 + 管理 API
    for (const auto &kv : m_agents)
        if (kv.second->cfg.enabled)
            spawnAgent(kv.first);
}

// ---------------------------------------------------------------------------
// frps 服务端(可选): 监听 bindPort 等待远端 frpc 连入做中转
// ---------------------------------------------------------------------------
void JZAgentCliServer::startFrp()
{
    if (!m_config.frpEnabled)
        return;
        
    JZFrpServerConfig sc;
    sc.token = m_config.frpToken;
    m_frpServer = new JZFrpServer(this);
    m_frpServer->setConfig(sc);
    if (!m_frpServer->listen(m_config.frpBindPort))
        jzWarning() << "FrpServer 监听失败:" << m_config.frpBindPort;
    else
        jzInfo() << "FrpServer(frps) 已启动, 监听:" << m_config.frpBindPort
                 << "  token=" << (sc.token.empty() ? "(无)" : "(已启用)");
}

// ---------------------------------------------------------------------------
// 控制面(hub): agent_cli 出站连进来(JZNet + token)
//   cli 是 client, 本进程是 server —— 内网机器不用 frp / 不用端口映射,
//   server 也不需要知道 cli 的地址(见 docs/agent-hub-design.md §2)。
// ---------------------------------------------------------------------------
void JZAgentCliServer::startHub()
{
    JZAgentHubPacketInit();

    m_hub = new JZNetServer(this);
    if (!m_config.hub.token.empty())
        m_hub->setToken(m_config.hub.token);

    connect(&m_hub->sigAccept, this, [this]()
    {
        // 握手完成才会 emit sigAccept; 这里只把待处理队列排空(否则会一直堆着)
        while (true)
        {
            uint64_t conn = m_hub ? m_hub->nextPendingConnection() : 0;
            if (conn == 0)
                break;
            jzInfo().noquote().nospace() << "控制面: 新连接 " << conn << " (等 hello)";
        }
    });
    connect(&m_hub->sigPacketReceive, this, [this](uint64_t conn, JZNetPackPtr pack)
    {
        onHubPacket(conn, pack);
    });
    connect(&m_hub->sigDisconnected, this, [this](uint64_t conn)
    {
        onHubDisconnected(conn);
    });

    if (!m_hub->listen(m_config.hub.bind, m_config.hub.port))
        jzWarning() << "控制面监听失败:" << m_config.hub.bind << ":" << m_config.hub.port;
    else
        jzInfo() << "控制面已启动(JZNet):" << m_config.hub.bind << ":" << m_config.hub.port
                 << (m_config.hub.token.empty() ? "  token=(无)" : "  token=(已启用)");
}

void JZAgentCliServer::onHubPacket(uint64_t conn, JZNetPackPtr pack)
{
    if (!pack || !m_hub)
        return;

    const int type = pack->packetType();
    const int64_t now = JZDateTime::currentMSecsSinceEpoch();

    if (type == HubPack_hello)
    {
        JZHubPackHello *hello = (JZHubPackHello *)pack.get();
        if (hello->agentId.empty())
        {
            JZHubPackHelloAck ack;
            ack.ok = false;
            ack.error = "hello 缺 agentId";
            ack.serverTimeMs = now;
            m_hub->sendPacket(conn, &ack);
            m_hub->closeClient(conn);
            return;
        }

        // 同一个 agentId 又连上来(进程重启/网络切换): 让旧连接让位, 否则管理台会出现两行
        auto old = m_connByAgent.find(hello->agentId);
        if (old != m_connByAgent.end() && old->second != conn)
        {
            const uint64_t oldConn = old->second;
            m_onlineByConn.erase(oldConn);
            m_hub->closeClient(oldConn);
        }

        JZCliServerOnlineAgent agent;
        agent.conn = conn;
        agent.agentId = hello->agentId;
        agent.name = hello->name;
        agent.version = hello->version;
        agent.rootDir = hello->rootDir;
        agent.modelName = hello->modelName;
        agent.nodes = hello->nodes;
        agent.lastSeen = now;
        m_onlineByConn[conn] = agent;
        m_connByAgent[agent.agentId] = conn;

        JZHubPackHelloAck ack;
        ack.ok = true;
        ack.acceptTasks = true;
        ack.serverTimeMs = now;
        m_hub->sendPacket(conn, &ack);

        jzInfo().noquote().nospace() << "cli 上线: " << agent.agentId << " (" << agent.name
                                     << " v" << agent.version << ")  conn=" << conn;
        return;
    }

    auto it = m_onlineByConn.find(conn);
    if (it == m_onlineByConn.end())
    {
        jzWarning().noquote().nospace() << "控制面: 未 hello 的包(type=" << type << "), 已丢弃";
        return;
    }
    JZCliServerOnlineAgent &agent = it->second;
    agent.lastSeen = now;

    if (type == HubPack_heartbeat)
    {
        JZHubPackHeartbeat *hb = (JZHubPackHeartbeat *)pack.get();
        agent.busy = hb->busy;
        agent.round = hb->round;
        agent.taskId = hb->taskId;
        agent.nodes = hb->nodes;
        return;
    }

    if (type == HubPack_taskEvent)
    {
        JZHubPackTaskEvent *ev = (JZHubPackTaskEvent *)pack.get();
        agent.lastEventType = ev->type;
        agent.lastEventData = ev->data;
        return;
    }

    if (type == HubPack_taskEnd)
    {
        JZHubPackTaskEnd *end = (JZHubPackTaskEnd *)pack.get();
        agent.busy = false;
        agent.taskId.clear();

        auto pit = m_pendingTasks.find(end->taskId);
        if (pit == m_pendingTasks.end())
        {
            // 已经超时收口(或 server 重启过): 结果没人要了, 只记一条
            jzWarning().noquote().nospace() << "任务 " << end->taskId << " 的回包没有提交方(可能已超时)";
            return;
        }

        PendingTask pt = pit->second;
        m_pendingTasks.erase(pit);
        if (pt.timer)
        {
            pt.timer->stop();
            pt.timer->deleteLater();
        }

        Json::Value result;
        if (!parseJsonText(end->result, result) || !result.isObject())
        {
            Json::Value err;
            err["status"] = "failed";
            err["error"] = "cli 返回的任务结果不是合法 JSON";
            respondJsonAsync(pt.client, 500, err);
            return;
        }
        respondJsonAsync(pt.client, 200, result);
        jzInfo().noquote().nospace() << "任务完成: " << end->taskId << "  agent=" << pt.agentId
                                     << "  status=" << result.get("status", "").asString();
        return;
    }
}

void JZAgentCliServer::onHubDisconnected(uint64_t conn)
{
    auto it = m_onlineByConn.find(conn);
    if (it == m_onlineByConn.end())
        return;   // 未 hello 就断了

    const string agentId = it->second.agentId;
    m_onlineByConn.erase(it);
    auto ai = m_connByAgent.find(agentId);
    if (ai != m_connByAgent.end() && ai->second == conn)
        m_connByAgent.erase(ai);

    jzWarning().noquote().nospace() << "cli 掉线: " << agentId;
    failPendingTasksOf(agentId, "agent " + agentId + " 掉线, 任务结果未知");
}

void JZAgentCliServer::failPendingTasksOf(const string &agentId, const string &error)
{
    for (auto it = m_pendingTasks.begin(); it != m_pendingTasks.end(); )
    {
        if (it->second.agentId != agentId)
        {
            ++it;
            continue;
        }
        PendingTask pt = it->second;
        it = m_pendingTasks.erase(it);

        if (pt.timer)
        {
            pt.timer->stop();
            pt.timer->deleteLater();
        }
        Json::Value err;
        err["status"] = "failed";
        err["error"] = error;
        respondJsonAsync(pt.client, 502, err);
    }
}

Json::Value JZAgentCliServer::onlineAgentsJson() const
{
    Json::Value arr(Json::arrayValue);
    for (const auto &kv : m_onlineByConn)
    {
        const JZCliServerOnlineAgent &a = kv.second;
        Json::Value j(Json::objectValue);
        j["agentId"] = a.agentId;
        j["name"] = a.name;
        j["version"] = a.version;
        j["rootDir"] = a.rootDir;
        j["modelName"] = a.modelName;
        j["busy"] = a.busy;
        j["round"] = a.round;
        j["taskId"] = a.taskId;
        j["acceptTasks"] = a.acceptTasks;
        j["lastSeen"] = (Json::Int64)a.lastSeen;
        j["lastEventType"] = a.lastEventType;
        j["lastEvent"] = a.lastEventData;   // JSON 字符串(已按包上限裁过)
        Json::Value nodes;
        if (parseJsonText(a.nodes, nodes))
            j["nodes"] = nodes;
        else
            j["nodes"] = Json::Value(Json::arrayValue);
        arr.append(j);
    }
    return arr;
}

// ---------------------------------------------------------------------------
// 任务下发(POST /api/tasks): 与 POST /task 的字段完全一致, 外加 agentId
// ---------------------------------------------------------------------------
void JZAgentCliServer::onApiTasks(JZHttpServerClient *client, const JZHttpRequest &req)
{
    string err;
    if (!checkToken(req, err))
    {
        respondJson(client, req, 401, Json::Value(err));
        return;
    }

    Json::Value body;
    if (!parseJsonText(string(req.body().data(), req.body().size()), body) || !body.isObject())
    {
        respondJson(client, req, 400, Json::Value("body 必须是 JSON 对象"));
        return;
    }

    string agentId = body.get("agentId", "").asString();
    if (agentId.empty() && m_onlineByConn.size() == 1)
        agentId = m_onlineByConn.begin()->second.agentId;   // 只有一台就默认它
    if (agentId.empty())
    {
        Json::Value e(Json::objectValue);
        e["error"] = "缺少 agentId(当前在线 " + to_string(m_onlineByConn.size()) + " 台)";
        respondJson(client, req, 400, e);
        return;
    }

    auto ai = m_connByAgent.find(agentId);
    if (ai == m_connByAgent.end())
    {
        Json::Value e(Json::objectValue);
        e["error"] = "没有在线的 agent: " + agentId;
        Json::Value ids(Json::arrayValue);
        for (const auto &kv : m_connByAgent)
            ids.append(kv.first);
        e["online"] = ids;
        respondJson(client, req, 404, e);
        return;
    }

    JZCliServerOnlineAgent &agent = m_onlineByConn[ai->second];
    if (agent.busy)
    {
        Json::Value e(Json::objectValue);
        e["error"] = "agent 正忙: " + agentId;
        e["taskId"] = agent.taskId;
        e["round"] = agent.round;
        respondJson(client, req, 409, e);
        return;
    }

    const string taskId = "t" + to_string(JZDateTime::currentMSecsSinceEpoch()) + "-" + to_string(++m_taskSeq);

    int timeoutMs = body.get("task_timeout_ms", 900000).asInt();
    if (timeoutMs < 1000) timeoutMs = 1000;
    if (timeoutMs > 24 * 3600 * 1000) timeoutMs = 24 * 3600 * 1000;

    JZHubPackTaskSubmit submit;
    submit.taskId = taskId;
    string payload = Json::FastWriter().write(body);
    while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r'))
        payload.pop_back();
    submit.body = payload;

    PendingTask pt;
    pt.taskId = taskId;
    pt.agentId = agentId;
    pt.client = client;
    pt.startMs = JZDateTime::currentMSecsSinceEpoch();

    JZTimer *timer = new JZTimer(this);
    timer->setSingleShot(true);
    connect(&timer->sigTimeout, this, [this, taskId]()
    {
        auto pit = m_pendingTasks.find(taskId);
        if (pit == m_pendingTasks.end())
            return;
        PendingTask pt2 = pit->second;
        m_pendingTasks.erase(pit);

        // 先请 cli 停掉, 免得它在后面继续跑一个没人收结果的任务
        auto ai2 = m_connByAgent.find(pt2.agentId);
        if (ai2 != m_connByAgent.end() && m_hub)
        {
            JZHubPackTaskCancel cancel;
            cancel.taskId = taskId;
            m_hub->sendPacket(ai2->second, &cancel);
        }
        if (pt2.timer)
            pt2.timer->deleteLater();

        Json::Value e(Json::objectValue);
        e["status"] = "failed";
        e["error"] = "任务超时(server 侧), 已请求 agent 取消";
        e["taskId"] = taskId;
        respondJsonAsync(pt2.client, 504, e);
    });
    timer->start(timeoutMs);
    pt.timer = timer;

    // 提交方提前断开: 只丢回包通道, 任务照跑(cli 侧行为与 HTTP /task 一致)
    connect(static_cast<JZSignal<JZObject*>*>(&client->sigDestroyed), this, [this](JZObject *obj)
    {
        for (auto &kv : m_pendingTasks)
            if (kv.second.client == static_cast<JZHttpServerClient *>(obj))
                kv.second.client = nullptr;
    });

    m_pendingTasks[taskId] = pt;
    agent.busy = true;
    agent.taskId = taskId;
    agent.round = 0;

    m_hub->sendPacket(ai->second, &submit);
    jzInfo().noquote().nospace() << "任务下发: " << taskId << " -> " << agentId;

    // 同步等待式(与现有 POST /task 手感一致): 回包在 taskEnd 到达时由 onHubPacket 发出。
}

void JZAgentCliServer::onApiTaskCancel(JZHttpServerClient *client, const JZHttpRequest &req)
{
    string err;
    if (!checkToken(req, err))
    {
        respondJson(client, req, 401, Json::Value(err));
        return;
    }

    // 支持 ?taskId= 或 body {"taskId": "..."}
    string taskId = req.queryParam("taskId");
    if (taskId.empty())
    {
        Json::Value body;
        if (parseJsonText(string(req.body().data(), req.body().size()), body) && body.isObject())
            taskId = body.get("taskId", "").asString();
    }
    if (taskId.empty())
    {
        respondJson(client, req, 400, Json::Value("缺少 taskId"));
        return;
    }

    auto pit = m_pendingTasks.find(taskId);
    if (pit == m_pendingTasks.end())
    {
        Json::Value e(Json::objectValue);
        e["error"] = "没有这个在跑的任务: " + taskId;
        respondJson(client, req, 404, e);
        return;
    }

    auto ai = m_connByAgent.find(pit->second.agentId);
    if (ai != m_connByAgent.end() && m_hub)
    {
        JZHubPackTaskCancel cancel;
        cancel.taskId = taskId;
        m_hub->sendPacket(ai->second, &cancel);
    }

    Json::Value j(Json::objectValue);
    j["ok"] = true;
    j["taskId"] = taskId;
    respondJson(client, req, 202, j);
}

// ---------------------------------------------------------------------------
// 后台网页 + 管理 API
// ---------------------------------------------------------------------------
void JZAgentCliServer::startHttp()
{
    m_http = new JZHttpServer(this);

    // 管理 API(精确路由, 优先于兜底)
    m_http->addRoute("/api/status", new ApiStatusHandler(this));
    m_http->addRoute("/api/tasks/cancel", new ApiTaskCancelHandler(this));
    m_http->addRoute("/api/tasks", new ApiTasksHandler(this));
    m_http->addRoute("/api/agents", new ApiAgentsHandler(this));
    m_http->addRoute("/api/agents/logs", new ApiAgentLogsHandler(this));
    m_http->addRoute("/api/agents/start", new ApiAgentActionHandler(this, "start"));
    m_http->addRoute("/api/agents/stop", new ApiAgentActionHandler(this, "stop"));
    m_http->addRoute("/api/agents/restart", new ApiAgentActionHandler(this, "restart"));

    // 后台网页(含兜底, 未知路径也返回 dashboard, 方便深链)
    m_http->addRoute("/", new DashboardHandler(this));
    m_http->addRoute("/<path:path>", new DashboardHandler(this));

    if (!m_http->listen(m_config.http.bind, m_config.http.port))
        jzWarning() << "管理网页监听失败:" << m_config.http.bind << ":" << m_config.http.port;
    else
        jzInfo() << "管理网页已启动: http://" << m_config.http.bind << ":" << m_config.http.port
                 << (m_config.http.token.empty() ? "" : "  (需 token)");
}

// ---------------------------------------------------------------------------
// agent 管理
// ---------------------------------------------------------------------------
string JZAgentCliServer::resolveProgram(const JZCliServerAgentConfig &cfg) const
{
    string program = cfg.cmd;
    if (program.empty() || program == "agent_cli")
    {
        program = JZPlatform::applicationDirPath() + "/agent_cli";
#ifdef _WIN32
        if (!JZFileInfo::exists(program))
            program += ".exe";
#endif
    }
    return program;
}

bool JZAgentCliServer::spawnAgent(const string &name)
{
    auto it = m_agents.find(name);
    if (it == m_agents.end())
        return false;
    JZCliServerAgentInstance *inst = it->second;
    if (inst->state == JZProcess::Running || inst->state == JZProcess::Starting)
        return false;   // 已在跑

    string program = resolveProgram(inst->cfg);
    if (!inst->proc->start(program, inst->cfg.args))
    {
        appendLog(inst, "[error] 启动失败: " + program + "\n");
        return false;
    }
    inst->state = JZProcess::Starting;   // start 成功仅表示进程创建成功, Running 等 sigStarted
    inst->startTime = JZDateTime::currentMSecsSinceEpoch();
    inst->stopTime = 0;
    inst->exitCode = 0;
    jzInfo() << "agent 启动:" << name << "  cmd=" << program;
    return true;
}

bool JZAgentCliServer::stopAgent(const string &name)
{
    auto it = m_agents.find(name);
    if (it == m_agents.end())
        return false;
    JZCliServerAgentInstance *inst = it->second;
    if (inst->state != JZProcess::Running && inst->state != JZProcess::Starting)
        return false;
    inst->proc->terminate();
    jzInfo() << "agent 停止:" << name;
    return true;
}

bool JZAgentCliServer::restartAgent(const string &name)
{
    auto it = m_agents.find(name);
    if (it == m_agents.end())
        return false;
    JZCliServerAgentInstance *inst = it->second;
    if (inst->state == JZProcess::Running || inst->state == JZProcess::Starting)
    {
        inst->proc->killProcess();
        inst->proc->waitForFinished(2000);
    }
    {
        JZMutexLocker lock(&inst->mutex);
        inst->state = JZProcess::NotRunning;
        inst->recvBuf.clear();
    }
    return spawnAgent(name);
}

// ---------------------------------------------------------------------------
// 日志收集(读线程调用): 拆行入环形缓冲
// ---------------------------------------------------------------------------
void JZAgentCliServer::appendLog(JZCliServerAgentInstance *inst, const string &chunk)
{
    inst->recvBuf += chunk;
    size_t pos = 0;
    while (true)
    {
        size_t nl = inst->recvBuf.find('\n', pos);
        if (nl == string::npos)
            break;
        string line = inst->recvBuf.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        {
            JZMutexLocker lock(&inst->mutex);
            inst->logs.push_back(line);
            while (inst->logs.size() > inst->logMax)
                inst->logs.pop_front();
        }
        pos = nl + 1;
    }
    inst->recvBuf = inst->recvBuf.substr(pos);
}

// ---------------------------------------------------------------------------
// token 校验
// ---------------------------------------------------------------------------
bool JZAgentCliServer::checkToken(const JZHttpRequest &req, string &err) const
{
    if (m_config.http.token.empty())
        return true;
    string t = req.queryParam("token");
    if (t.empty())
    {
        string auth = req.header("Authorization");   // Authorization: Bearer <token>
        if (auth.compare(0, 7, "Bearer ") == 0)
            t = auth.substr(7);
    }
    if (t != m_config.http.token)
    {
        err = "token 错误/缺失";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 路由实现
// ---------------------------------------------------------------------------
void JZAgentCliServer::onApiStatus(JZHttpServerClient *client, const JZHttpRequest &req)
{
    string err;
    if (!checkToken(req, err))
    {
        respondJson(client, req, 401, Json::Value(err));
        return;
    }
    Json::Value j;
    j["frp"]["enabled"] = m_config.frpEnabled;
    j["frp"]["bindPort"] = m_config.frpBindPort;
    j["http"]["bind"] = m_config.http.bind;
    j["http"]["port"] = m_config.http.port;
    j["hub"]["bind"] = m_config.hub.bind;
    j["hub"]["port"] = m_config.hub.port;
    j["hub"]["listening"] = (m_hub != nullptr && m_hub->isListening());
    j["uptimeMs"] = (int)(JZDateTime::currentMSecsSinceEpoch() - m_startTime);
    Json::Value arr = Json::arrayValue;
    for (auto &kv : m_agents)
        arr.append(agentToJson(kv.second));
    j["agents"] = arr;
    // 控制面已上线的 cli(托管模式): 与"本机子进程"是两个来源, 分开列
    j["online"] = onlineAgentsJson();
    respondJson(client, req, 200, j);
}

void JZAgentCliServer::onApiAgents(JZHttpServerClient *client, const JZHttpRequest &req)
{
    string err;
    if (!checkToken(req, err))
    {
        respondJson(client, req, 401, Json::Value(err));
        return;
    }
    Json::Value j;
    Json::Value arr = Json::arrayValue;
    for (auto &kv : m_agents)
        arr.append(agentToJson(kv.second));
    j["agents"] = arr;
    j["online"] = onlineAgentsJson();
    respondJson(client, req, 200, j);
}

void JZAgentCliServer::onApiAgentLogs(JZHttpServerClient *client, const JZHttpRequest &req)
{
    string err;
    if (!checkToken(req, err))
    {
        respondJson(client, req, 401, Json::Value(err));
        return;
    }
    string name = req.queryParam("name");
    auto it = m_agents.find(name);
    if (it == m_agents.end())
    {
        respondJson(client, req, 404, Json::Value("no such agent"));
        return;
    }
    int lines = atoi(req.queryParam("lines").c_str());
    if (lines <= 0) lines = 200;
    if (lines > 5000) lines = 5000;

    string out;
    {
        JZMutexLocker lock(&it->second->mutex);
        auto &logs = it->second->logs;
        size_t start = logs.size() > (size_t)lines ? logs.size() - lines : 0;
        for (size_t i = start; i < logs.size(); ++i)
        {
            out += logs[i];
            out += "\n";
        }
    }
    respondText(client, req, 200, out);   // text/plain 直接给前端展示
}

void JZAgentCliServer::onApiAgentAction(JZHttpServerClient *client, const JZHttpRequest &req,
                                         const string &action)
{
    string err;
    if (!checkToken(req, err))
    {
        respondJson(client, req, 401, Json::Value(err));
        return;
    }
    string name = req.queryParam("name");
    bool ok = false;
    string msg;
    if (name.empty())
        msg = "缺少 name";
    else if (action == "start")
    {
        ok = spawnAgent(name);
        msg = ok ? "已启动" : "启动失败/已在运行";
    }
    else if (action == "stop")
    {
        ok = stopAgent(name);
        msg = ok ? "已停止" : "未在运行";
    }
    else if (action == "restart")
    {
        ok = restartAgent(name);
        msg = ok ? "已重启" : "重启失败";
    }
    else
        msg = "未知动作: " + action;

    Json::Value j;
    j["ok"] = ok;
    j["msg"] = msg;
    respondJson(client, req, ok ? 200 : 400, j);
}

void JZAgentCliServer::onDashboard(JZHttpServerClient *client, const JZHttpRequest &req)
{
    string err;
    if (!checkToken(req, err))
    {
        JZHttpRespone resp;
        resp.setStatusCode(401);
        resp.setContentType("text/plain; charset=utf-8");
        resp.setBody("需要 token: 访问 /?token=YOUR_TOKEN");
        client->response(resp, req);
        return;
    }
    respondText(client, req, 200, buildDashboardHtml(), "text/html; charset=utf-8");
}

// ---------------------------------------------------------------------------
// 后台网页(单文件 SPA, 内嵌; 轮询 /api/* 显示 agent 状态与日志)
// ---------------------------------------------------------------------------
string JZAgentCliServer::buildDashboardHtml() const
{
    return R"JZSERVERHTML(<!DOCTYPE html>
<html lang="zh">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>JZAgentCliServer 管理台</title>
<style>
  :root { --bg:#0f1115; --panel:#171a21; --line:#262b36; --fg:#e6e9ef; --mut:#9aa3b2;
          --grn:#3fb950; --red:#f85149; --yel:#d29922; --blu:#58a6ff; }
  * { box-sizing:border-box; }
  body { margin:0; background:var(--bg); color:var(--fg);
         font:14px/1.5 -apple-system,Segoe UI,Roboto,Helvetica,Arial,sans-serif; }
  header { padding:14px 18px; border-bottom:1px solid var(--line); display:flex;
           align-items:center; gap:14px; flex-wrap:wrap; }
  header h1 { font-size:16px; margin:0; font-weight:600; }
  .sub { color:var(--mut); font-size:12px; }
  .wrap { padding:18px; }
  .cards { display:flex; gap:14px; flex-wrap:wrap; margin-bottom:18px; }
  .card { background:var(--panel); border:1px solid var(--line); border-radius:10px;
          padding:12px 16px; min-width:140px; }
  .card .k { color:var(--mut); font-size:12px; }
  .card .v { font-size:20px; font-weight:600; margin-top:4px; }
  table { width:100%; border-collapse:collapse; background:var(--panel);
          border:1px solid var(--line); border-radius:10px; overflow:hidden; }
  th,td { text-align:left; padding:10px 12px; border-bottom:1px solid var(--line); }
  th { color:var(--mut); font-weight:600; font-size:12px; background:#11141a; }
  tr:last-child td { border-bottom:none; }
  tr.sel { background:#1c2733; cursor:pointer; }
  .badge { display:inline-block; padding:2px 8px; border-radius:999px; font-size:12px;
           border:1px solid var(--line); }
  .badge.running { color:var(--grn); border-color:var(--grn); }
  .badge.stopped { color:var(--mut); }
  .badge.starting { color:var(--yel); border-color:var(--yel); }
  button { background:#21262d; color:var(--fg); border:1px solid var(--line);
           border-radius:6px; padding:5px 10px; cursor:pointer; font-size:13px; }
  button:hover { border-color:var(--blu); }
  button.danger:hover { border-color:var(--red); color:var(--red); }
  .logbar { display:flex; align-items:center; gap:10px; margin:16px 0 8px; }
  .logbar .name { font-weight:600; }
  pre.log { background:#0b0d12; border:1px solid var(--line); border-radius:10px;
            padding:12px; height:48vh; overflow:auto; white-space:pre-wrap;
            word-break:break-all; font:12.5px/1.45 ui-monospace,SFMono-Regular,Menlo,monospace; }
  .empty { color:var(--mut); }
</style>
</head>
<body>
<header>
  <h1>JZAgentCliServer 管理台</h1>
  <span class="sub" id="meta"></span>
</header>
<div class="wrap">
  <div class="cards" id="cards"></div>
  <h3 class="sub" style="margin:0 0 8px">控制面(agent_cli 出站接入)</h3>
  <table id="onlineTbl">
    <thead><tr><th>agentId</th><th>名称</th><th>状态</th><th>节点</th><th>最近事件</th><th>工作区</th></tr></thead>
    <tbody></tbody>
  </table>
  <h3 class="sub" style="margin:18px 0 8px">本机托管的 agent_cli 子进程</h3>
  <table id="tbl">
    <thead><tr><th>名称</th><th>状态</th><th>退出码</th><th>命令</th><th>日志行</th><th>操作</th></tr></thead>
    <tbody></tbody>
  </table>

  <div class="logbar">
    <span class="name" id="logName">未选择 agent</span>
    <span class="sub">每 1.5s 刷新</span>
    <button id="copyToken" style="margin-left:auto">复制带 token 的链接</button>
  </div>
  <pre class="log" id="log"><span class="empty">点上方某一行查看其日志</span></pre>
</div>

<script>
const params = new URLSearchParams(location.search);
const TOKEN = params.get('token') || '';
const Q = TOKEN ? ('?token=' + encodeURIComponent(TOKEN)) : '';
const api = (p) => '/api/' + p + (TOKEN ? (p.includes('?') ? '&' : '?') + 'token=' + encodeURIComponent(TOKEN) : '');

let selected = null;

async function load() {
  try {
    const r = await fetch(api('status'));
    if (!r.ok) { document.getElementById('meta').textContent = '未授权/错误 ' + r.status; return; }
    const s = await r.json();
    renderMeta(s);
    renderCards(s);
    renderOnline(s);
    renderTable(s);
  } catch (e) { document.getElementById('meta').textContent = '请求失败: ' + e; }
}

function renderMeta(s) {
  const up = ((s.uptimeMs||0)/1000).toFixed(0);
  const hub = s.hub || {};
  document.getElementById('meta').textContent =
    `frps=${s.frp.enabled?'开':'关'}  网页=http://${s.http.bind}:${s.http.port}` +
    `  控制面=${hub.listening?('监听 '+hub.port):'关'}  在线 cli ${(s.online||[]).length}` +
    `  子进程 ${s.agents.length}  运行 ${up}s`;
}

// 控制面上线的 cli: 一行一台, 节点列显示 node 在线数/总数
function renderOnline(s) {
  const tb = document.querySelector('#onlineTbl tbody');
  const list = s.online || [];
  if (!list.length) {
    tb.innerHTML = '<tr><td colspan="6" class="empty">还没有 cli 连上来(在 agent_cli 配置里填 server.host/port/token)</td></tr>';
    return;
  }
  tb.innerHTML = list.map(a => {
    const nodes = a.nodes || [];
    const on = nodes.filter(n => n.online).length;
    const state = a.busy ? `<span class="badge running">跑 ${esc(a.taskId||'')}</span>`
                         : '<span class="badge stopped">空闲</span>';
    const ev = a.lastEventType ? `${esc(a.lastEventType)}` : '<span class="empty">-</span>';
    return `<tr><td>${esc(a.agentId)}</td><td>${esc(a.name)} <span class="empty">v${esc(a.version)}</span></td>` +
           `<td>${state}</td>` +
           `<td>${nodes.length? (on+'/'+nodes.length) : '<span class="empty">本地</span>'}</td>` +
           `<td>${ev}</td><td class="empty">${esc(a.rootDir||'')}</td></tr>`;
  }).join('');
}

function renderCards(s) {
  const running = s.agents.filter(a=>a.running).length;
  const cards = [
    [' Agents', s.agents.length],
    [' 运行中', running],
    [' frps', s.frp.enabled ? ('监听 '+s.frp.bindPort) : '关'],
    [' 运行时间', ((s.uptimeMs||0)/1000).toFixed(0)+'s'],
  ];
  document.getElementById('cards').innerHTML = cards.map(c=>
    `<div class="card"><div class="k">${c[0]}</div><div class="v">${c[1]}</div></div>`).join('');
}

function renderTable(s) {
  const tb = document.querySelector('#tbl tbody');
  tb.innerHTML = '';
  s.agents.forEach(a => {
    const tr = document.createElement('tr');
    if (a.name === selected) tr.className = 'sel';
    tr.innerHTML =
      `<td>${esc(a.name)}</td>` +
      `<td><span class="badge ${a.state}">${a.state}</span></td>` +
      `<td>${a.exitCode}</td>` +
      `<td class="empty">${esc((a.cmd)+' '+(a.args||[]).join(' '))}</td>` +
      `<td>${a.logLines}</td>` +
      `<td>
         <button onclick="act('start','${esc(a.name)}',event)">启动</button>
         <button onclick="act('stop','${esc(a.name)}',event)">停止</button>
         <button class="danger" onclick="act('restart','${esc(a.name)}',event)">重启</button>
       </td>`;
    tr.onclick = (e) => { if (e.target.tagName==='BUTTON') return; selected = a.name; loadLog(); load(); };
    tb.appendChild(tr);
  });
  if (selected && !s.agents.find(a=>a.name===selected)) { selected=null; }
}

async function act(action, name, e) {
  e.stopPropagation();
  const r = await fetch(api(`agents/${action}?name=${encodeURIComponent(name)}`), {method:'POST'});
  const j = await r.json().catch(()=>({msg:''}));
  load();
  if (action!=='stop') setTimeout(loadLog, 600);
}

async function loadLog() {
  if (!selected) return;
  document.getElementById('logName').textContent = '日志: ' + selected;
  try {
    const r = await fetch(api(`agents/logs?name=${encodeURIComponent(selected)}&lines=300`));
    if (!r.ok) { document.getElementById('log').textContent = '(无日志/未授权)'; return; }
    const t = await r.text();
    const pre = document.getElementById('log');
    pre.textContent = t || '(空)';
    pre.scrollTop = pre.scrollHeight;
  } catch (e) { document.getElementById('log').textContent = '日志请求失败: ' + e; }
}

function esc(s){ return String(s).replace(/[&<>"]/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c])); }

document.getElementById('copyToken').onclick = () => {
  const url = location.origin + '/?token=' + encodeURIComponent(TOKEN);
  navigator.clipboard?.writeText(url);
  document.getElementById('copyToken').textContent = '已复制';
  setTimeout(()=>document.getElementById('copyToken').textContent='复制带 token 的链接', 1500);
};

load();
setInterval(load, 2000);
setInterval(() => { if (selected) loadLog(); }, 1500);
</script>
</body>
</html>
)JZSERVERHTML";
}
