#include "JZAgentCliServer.h"

#include "JZNet/JZNetServer.h"   // 控制面(hub): cli 出站连进来的 JZNetServer
#include "JZAgentHubPacket.h"    // 控制面协议(cli <-> server)
#include "JZHttpServer.h"        // 后台网页 / 管理 API
#include "JZHttpStaticFiles.h"
#include "JZProcess.h"            // 拉起并监管 agent_cli 子进程
#include "JZDir.h"               // mkpath(data 目录)
#include "JZPlatform.h"          // applicationDirPath
#include "JZFileInfo.h"          // exists
#include "JZMutex.h"             // JZMutex / JZMutexLocker(日志跨线程读写保护)
#include "JZDateTime.h"          // currentMSecsSinceEpoch
#include "JZTimer.h"             // 任务超时(到点回 504 并请求 cli 取消)
#include "JZDebug.h"
#include "JZJson.h"              // Json::Value / FastWriter

#include <algorithm>
#include <cstdio>        // snprintf(SSE chunked 长度前缀)
#include <ctime>         // gmtime / strftime(TaskStatus.timestamp)
#include <deque>
#include <fstream>       // 管理台页面从磁盘读(server_web/index.html)
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
    void handle(JZHttpServerClient *c, const JZHttpRequest &r) override
    {
        // 同一个路径两种方法: GET = 历史列表, POST = 新建任务
        if (r.methodString() == "GET")
            m_s->onApiTaskList(c, r);
        else
            m_s->onApiTasks(c, r);
    }
private:
    JZAgentCliServer *m_s;
};

class ApiTaskDetailHandler : public JZHttpHandler
{
public:
    explicit ApiTaskDetailHandler(JZAgentCliServer *s) : m_s(s) {}
    void handle(JZHttpServerClient *c, const JZHttpRequest &r) override { m_s->onApiTaskDetail(c, r); }
private:
    JZAgentCliServer *m_s;
};

class ApiAgentDetailHandler : public JZHttpHandler
{
public:
    explicit ApiAgentDetailHandler(JZAgentCliServer *s) : m_s(s) {}
    void handle(JZHttpServerClient *c, const JZHttpRequest &r) override { m_s->onApiAgentDetail(c, r); }
private:
    JZAgentCliServer *m_s;
};

class ApiAgentForgetHandler : public JZHttpHandler
{
public:
    explicit ApiAgentForgetHandler(JZAgentCliServer *s) : m_s(s) {}
    void handle(JZHttpServerClient *c, const JZHttpRequest &r) override { m_s->onApiAgentForget(c, r); }
private:
    JZAgentCliServer *m_s;
};

// 静态页: 一个页面一个文件(server_web/<name>), 不再是一个页面塞所有东西
class PageHandler : public JZHttpHandler
{
public:
    PageHandler(JZAgentCliServer *s, const std::string &file) : m_s(s), m_file(file) {}
    void handle(JZHttpServerClient *c, const JZHttpRequest &r) override { m_s->servePage(c, r, m_file); }
private:
    JZAgentCliServer *m_s;
    std::string m_file;
};

class ApiTaskCancelHandler : public JZHttpHandler
{
public:
    explicit ApiTaskCancelHandler(JZAgentCliServer *s) : m_s(s) {}
    void handle(JZHttpServerClient *c, const JZHttpRequest &r) override { m_s->onApiTaskCancel(c, r); }
private:
    JZAgentCliServer *m_s;
};

class ApiTaskSubscribeHandler : public JZHttpHandler
{
public:
    explicit ApiTaskSubscribeHandler(JZAgentCliServer *s) : m_s(s) {}
    void handle(JZHttpServerClient *c, const JZHttpRequest &r) override { m_s->onApiTaskSubscribe(c, r); }
private:
    JZAgentCliServer *m_s;
};

class ApiAgentCardHandler : public JZHttpHandler
{
public:
    explicit ApiAgentCardHandler(JZAgentCliServer *s) : m_s(s) {}
    void handle(JZHttpServerClient *c, const JZHttpRequest &r) override { m_s->onApiAgentCard(c, r); }
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

// ---------------------------------------------------------------------------
// A2A v1.0 在管理台侧的落地辅助
//   - 内部状态词(running/success/...)一个不改, 只在输出时映射成 TaskState;
//   - events / artifacts / history 在内存里存的是"一行 JSON 字符串", 落盘时组装成数组;
//   - SSE 用 chunked 分块(与 agent_cli 的 /events 同口径), 浏览器与非浏览器客户端都能逐帧读。
// ---------------------------------------------------------------------------
// 只有一套状态: A2A TaskState。cli 上报的 success/failed/cancelled 只在边界映射一次。
static const char *kStateSubmitted = "TASK_STATE_SUBMITTED";
static const char *kStateWorking = "TASK_STATE_WORKING";
static const char *kStateCompleted = "TASK_STATE_COMPLETED";
static const char *kStateFailed = "TASK_STATE_FAILED";
static const char *kStateCanceled = "TASK_STATE_CANCELED";
static const char *kStateRejected = "TASK_STATE_REJECTED";
static const char *kStateUnknown = "TASK_STATE_UNSPECIFIED";

static bool isWorkingState(const string &s)
{
    return s == kStateSubmitted || s == kStateWorking ||
           s == "TASK_STATE_INPUT_REQUIRED" || s == "TASK_STATE_AUTH_REQUIRED";
}

static string stateFromResult(const string &status)
{
    if (status == "success")   return kStateCompleted;
    if (status == "cancelled") return kStateCanceled;
    if (status == "rejected")  return kStateRejected;
    if (status == "failed" || status == "timeout") return kStateFailed;
    return kStateUnknown;      // lost / 未知: 结果不确定, 不能当成 completed
}

static string isoTime(long long ms)
{
    if (ms <= 0)
        return string();
    time_t sec = (time_t)(ms / 1000);
    tm tmv;
#ifdef _WIN32
    gmtime_s(&tmv, &sec);
#else
    gmtime_r(&sec, &tmv);
#endif
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmv);
    return string(buf);
}

// A2A SendMessage 的 Message -> cli /task 需要的纯文本指令(只在这里转换一次)
static string messageText(const Json::Value &body)
{
    const Json::Value &msg = body.get("message", Json::Value::null);
    if (msg.isObject())
    {
        const Json::Value &parts = msg.get("parts", Json::Value::null);
        if (parts.isArray())
        {
            string text;
            for (Json::ArrayIndex i = 0; i < parts.size(); ++i)
            {
                const string one = parts[i].get("text", "").asString();
                if (!text.empty() && !one.empty()) text += "\n";
                text += one;
            }
            return text;
        }
    }
    return string();
}

static string toJsonLine(const Json::Value &v)
{
    string s = Json::FastWriter().write(v);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
        s.pop_back();
    return s;
}

static Json::Value jsonStringArray(const vector<string> &items)
{
    Json::Value arr(Json::arrayValue);
    for (const auto &s : items)
    {
        Json::Value v;
        arr.append(parseJsonText(s, v) ? v : Json::Value(s));
    }
    return arr;
}

static vector<string> stringsFromJson(const Json::Value &arr)
{
    vector<string> out;
    if (!arr.isArray())
        return out;
    for (Json::ArrayIndex i = 0; i < arr.size(); ++i)
    {
        const Json::Value &v = arr[i];
        out.push_back(v.isString() ? v.asString() : toJsonLine(v));
    }
    return out;
}

// hub 包里的 data 可能是 JSON 对象/字符串, 也可能被上游裁过而不合法; 一律归成 Value,
// 文本太长就截断(事件序列要落盘, 一条巨型日志会撑爆 tasks.jsonl)。
static Json::Value looseJson(const string &text)
{
    if (text.empty())
        return Json::Value(Json::objectValue);
    Json::Value v;
    if (parseJsonText(text, v))
        return v;
    return Json::Value(text.size() > 4000 ? text.substr(0, 4000) + "…" : text);
}

static string makeSseFrame(const string &event, const string &payload)
{
    string frame;
    if (!event.empty())
        frame += "event: " + event + "\n";
    string data;
    data.reserve(payload.size());
    for (char c : payload)
        data += (c == '\n' || c == '\r') ? ' ' : c;   // SSE 帧里 data 必须单行
    frame += "data: " + data + "\n\n";
    return frame;
}

static string sseChunk(const string &frame)
{
    char head[32];
    snprintf(head, sizeof(head), "%x\r\n", (unsigned)frame.size());
    return string(head) + frame + "\r\n";
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

// 整文件读成文本(证书/私钥/CA 都是 PEM 文本); 读不到返回 false
static bool readTextFile(const string &path, string &out)
{
    out.clear();
    ifstream ifs(path, ios::binary);
    if (!ifs)
        return false;
    stringstream ss;
    ss << ifs.rdbuf();
    out = ss.str();
    return !out.empty();
}

bool JZAgentCliServer::init(const JZCliServerConfig &config, std::string &error)
{
    m_config = config;

    // HTTPS 证书: 页面/API 默认走 TLS 且默认要求客户端证书, 所以这里先读进内存。
    // 读不到直接失败(而不是退回明文 HTTP): 静默降级等于把"起停 agent + 下发任务"的执行权
    // 暴露成裸 HTTP, 现场根本看不出来。
    m_httpCertPem.clear();
    m_httpKeyPem.clear();
    m_httpCaPem.clear();
    if (m_config.http.sslEnabled)
    {
        if (!readTextFile(m_config.http.certFile, m_httpCertPem))
        {
            error = "HTTPS 已启用, 但读不到服务器证书: " + m_config.http.certFile;
            return false;
        }
        if (!readTextFile(m_config.http.keyFile, m_httpKeyPem))
        {
            error = "HTTPS 已启用, 但读不到服务器私钥: " + m_config.http.keyFile;
            return false;
        }
        if (m_config.http.requireClientCert && !readTextFile(m_config.http.caFile, m_httpCaPem))
        {
            error = "HTTPS 要求校验客户端证书, 但读不到客户端 CA: " + m_config.http.caFile;
            return false;
        }
    }

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

    // 内置 agent: server 自己那个 JZAgent(可选)。先起它, 后面的工具才拿得到集群视图
    if (m_config.selfAgent.enabled)
    {
        m_selfAgent = new JZServerAgent(this);
        string err;
        if (!m_selfAgent->init(m_config.selfAgent, this, err))
        {
            jzWarning() << "内置 agent 启动失败:" << err;
            m_selfAgent->deleteLater();
            m_selfAgent = nullptr;
        }
        else
        {
            connect(&m_selfAgent->sigEvent, this, &JZAgentCliServer::onSelfAgentEvent);
            jzInfo() << "内置 agent 已就绪: id=" << m_config.selfAgent.agentId
                     << "  model=" << m_config.selfAgent.modelName
                     << "  root=" << m_config.selfAgent.rootDir;
        }
    }

    startHub();    // 控制面(JZNet): 等 agent_cli 出站连进来
    startHttp();   // 后台网页 + 管理 API
    for (const auto &kv : m_agents)
        if (kv.second->cfg.enabled)
            spawnAgent(kv.first);
}

// ---------------------------------------------------------------------------
// 控制面(hub): agent_cli 出站连进来(JZNet + token)
//   cli 是 client, 本进程是 server —— 内网机器不需要端口暴露/端口映射,
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

    // 心跳回收定时器(5s 一次; 判定阈值 20s 见 reapDeadAgents)
    m_reapTimer = new JZTimer(this);
    connect(&m_reapTimer->sigTimeout, this, [this]() { reapDeadAgents(); });
    m_reapTimer->start(5000);

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
            // 先走统一下线收尾, 再关旧连接; 直接 erase 会让旧任务永远停在 running。
            dropAgent(oldConn, "被同 agentId 的新连接替换");
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

        // 记进"曾经连过"的档案(离线后列表仍然认得出这台)
        KnownAgent known;
        known.agentId = agent.agentId;
        known.name = agent.name;
        known.version = agent.version;
        known.rootDir = agent.rootDir;
        known.modelName = agent.modelName;
        known.lastSeen = now;
        upsertKnown(known);

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
        recordEvent(ev->taskId, ev->type, ev->data);   // A2A: 事件进 Task.events + SSE 广播
        return;
    }

    if (type == HubPack_taskEnd)
    {
        JZHubPackTaskEnd *end = (JZHubPackTaskEnd *)pack.get();
        agent.busy = false;
        agent.taskId.clear();

        Json::Value result;
        if (!parseJsonText(end->result, result) || !result.isObject())
        {
            result = Json::Value(Json::objectValue);
            result["status"] = "failed";
            result["error"] = "cli 返回的任务结果不是合法 JSON";
        }
        // 先落记录: 即使提交方早就断了(超时/异步模式), 历史也要留全
        recordEnd(end->taskId, result);

        auto pit = m_pendingTasks.find(end->taskId);
        if (pit == m_pendingTasks.end())
        {
            jzInfo().noquote().nospace() << "任务结束(无提交方在等): " << end->taskId;
            return;
        }

        PendingTask pt = pit->second;
        m_pendingTasks.erase(pit);
        if (pt.timer)
        {
            pt.timer->stop();
            pt.timer->deleteLater();
        }

        // 同步等待方拿到的也是 A2A Task, 与 GetTask / SubscribeToTask 同一形状
        TaskRecord *rec = findTask(end->taskId);
        respondJsonAsync(pt.client, 200, rec ? taskJson(*rec) : Json::Value(Json::objectValue));
        jzInfo().noquote().nospace() << "任务完成: " << end->taskId << "  agent=" << pt.agentId
                                     << "  state=" << (rec ? rec->state : string(kStateUnknown));
        return;
    }
}

// 一台 agent 下线(两条来路: socket 断开 / 心跳超时), 收口逻辑只写一份
void JZAgentCliServer::dropAgent(uint64_t conn, const string &reason)
{
    auto it = m_onlineByConn.find(conn);
    if (it == m_onlineByConn.end())
        return;   // 未 hello 就断了

    const string agentId = it->second.agentId;
    m_onlineByConn.erase(it);
    auto ai = m_connByAgent.find(agentId);
    if (ai != m_connByAgent.end() && ai->second == conn)
        m_connByAgent.erase(ai);

    jzWarning().noquote().nospace() << "cli 掉线: " << agentId << " (" << reason << ")";
    failPendingTasksOf(agentId, "agent " + agentId + " " + reason + ", 任务结果未知");

    // 还在非终态(异步模式没有提交方在等)的任务也要标死: 否则历史里会永远停在 working
    vector<string> running;
    for (const auto &kv : m_tasks)
        if (kv.second.agentId == agentId && isWorkingState(kv.second.state))
            running.push_back(kv.first);
    for (const auto &id : running)
        recordFail(id, kStateUnknown, "agent " + agentId + " " + reason + ", 结果未知");
}

void JZAgentCliServer::onHubDisconnected(uint64_t conn)
{
    dropAgent(conn, "连接断开");
}

// 心跳回收: 控制面不能只靠 socket 断开 —— 拔网线/休眠/被防火墙丢包时 TCP 还是"通"的,
// 但对端早就不干活了。cli 每 5s 一次心跳, 超过 20s(4 次没到)就按掉线处理。
void JZAgentCliServer::reapDeadAgents()
{
    const int64_t now = JZDateTime::currentMSecsSinceEpoch();
    vector<uint64_t> dead;
    for (const auto &kv : m_onlineByConn)
        if (now - kv.second.lastSeen > 20000)
            dead.push_back(kv.first);

    for (uint64_t conn : dead)
    {
        dropAgent(conn, "心跳超时(>20s)");
        if (m_hub)
            m_hub->closeClient(conn);   // 顺手把死连接关掉, 免得一直占 fd(已断开时是 no-op)
    }
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
        recordFail(pt.taskId, kStateUnknown, error);   // 对提交方是 502; 历史必须保留"结果未知"
        TaskRecord *rec = findTask(pt.taskId);
        respondJsonAsync(pt.client, 502, rec ? taskJson(*rec) : Json::Value(Json::objectValue));
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

    // 内置 agent: 与远端 cli 平级地出现在列表里(网页点进去看到的就是它)
    if (m_selfAgent)
    {
        Json::Value j(Json::objectValue);
        j["agentId"] = m_config.selfAgent.agentId;
        j["name"] = m_config.selfAgent.name;
        j["version"] = "builtin";
        j["rootDir"] = m_selfAgent->config().rootDir;
        j["modelName"] = m_config.selfAgent.modelName;
        j["busy"] = m_selfAgent->busy();
        j["round"] = m_selfAgent->round();
        j["taskId"] = m_selfAgent->taskId();
        j["acceptTasks"] = true;
        j["builtin"] = true;
        j["lastSeen"] = (Json::Int64)JZDateTime::currentMSecsSinceEpoch();
        j["lastEventType"] = m_selfAgent->lastEventType();
        j["nodes"] = Json::Value(Json::arrayValue);
        arr.append(j);
    }
    return arr;
}

// ---------------------------------------------------------------------------
// 任务记录: server 侧历史
//   数据归属(§15): server 记"任务生命周期", cli 记"当前执行"; 历史必须在 server 落盘,
//   否则 cli 重启后提交方查不到自己交过的任务。JSONL 追加写, 载入时同 taskId 后者覆盖前者。
// ---------------------------------------------------------------------------
void JZAgentCliServer::loadTaskStore()
{
    const string dir = JZPlatform::applicationDirPath() + "/data";
    JZDir dataDir;
    dataDir.mkpath(dir);
    m_taskFile = dir + "/tasks.jsonl";

    ifstream ifs(m_taskFile, ios::binary);
    if (!ifs)
        return;

    const long long now = (long long)JZDateTime::currentMSecsSinceEpoch();
    string line;
    int n = 0;
    while (getline(ifs, line))
    {
        if (line.empty())
            continue;
        Json::Value j;
        if (!parseJsonText(line, j) || !j.isObject())
            continue;
        // 只认 A2A Task 一种落盘格式; 不是这种形状的行直接跳过, 不做旧格式兼容
        TaskRecord r;
        r.taskId = j.get("id", "").asString();
        if (r.taskId.empty())
            continue;
        Json::Value meta = j.get("metadata", Json::Value(Json::objectValue));
        Json::Value st = j.get("status", Json::Value(Json::objectValue));
        Json::Value statusMsg = st.get("message", Json::Value(Json::objectValue));
        Json::Value statusParts = statusMsg.get("parts", Json::Value(Json::arrayValue));
        r.agentId = meta.get("agentId", "").asString();
        r.prompt = meta.get("prompt", "").asString();
        r.verifyCmd = meta.get("verifyCmd", "").asString();
        r.state = st.get("state", kStateUnknown).asString();
        if (statusParts.isArray() && statusParts.size() > 0)
            r.error = statusParts[0].get("text", "").asString();
        r.lastEventType = meta.get("lastEventType", "").asString();
        r.rounds = meta.get("rounds", 0).asInt();
        r.round = meta.get("round", 0).asInt();
        r.startMs = (long long)meta.get("startMs", 0).asInt64();
        r.endMs = (long long)meta.get("endMs", 0).asInt64();
        r.contextId = j.get("contextId", "").asString();
        if (r.contextId.empty())
            r.contextId = "ctx-" + r.taskId;
        r.statusMs = (long long)meta.get("statusMs", r.endMs ? r.endMs : r.startMs).asInt64();
        r.events = stringsFromJson(meta.get("events", Json::Value(Json::arrayValue)));
        r.artifacts = stringsFromJson(j.get("artifacts", Json::Value(Json::arrayValue)));
        r.history = stringsFromJson(j.get("history", Json::Value(Json::arrayValue)));
        r.eventSeq = meta.get("eventSeq", (int)r.events.size()).asInt();
        if (m_tasks.find(r.taskId) == m_tasks.end())
            m_taskOrder.push_back(r.taskId);
        if (isWorkingState(r.state))
        {
            // 上次进程没等到回包就退出了: 结果不可知(不能假装还在跑)
            r.state = kStateUnknown;
            r.error = "server 重启, 结果未知";
            r.endMs = now;
        }
        m_tasks[r.taskId] = r;
        ++n;
    }
    while (m_taskOrder.size() > 500)
    {
        m_tasks.erase(m_taskOrder.front());
        m_taskOrder.erase(m_taskOrder.begin());
    }
    jzInfo().noquote().nospace() << "任务记录: 载入 " << n << " 条 (" << m_taskFile << ")";
}

void JZAgentCliServer::saveTaskRecord(const TaskRecord &r)
{
    if (m_taskFile.empty())
        return;
    string line = Json::FastWriter().write(taskJson(r));
    ofstream ofs(m_taskFile, ios::app | ios::binary);
    if (!ofs)
    {
        jzWarning() << "任务记录写不进去:" << m_taskFile;
        return;
    }
    ofs << line;   // FastWriter 自带行尾
}

void JZAgentCliServer::rewriteTaskStore()
{
    if (m_taskFile.empty())
        return;
    ofstream ofs(m_taskFile, ios::binary | ios::trunc);
    if (!ofs)
    {
        jzWarning() << "任务记录重写失败:" << m_taskFile;
        return;
    }
    // 保持 JSONL: 每条一行, 与追加写同格式
    for (const auto &id : m_taskOrder)
    {
        auto it = m_tasks.find(id);
        if (it == m_tasks.end())
            continue;
        string line = Json::FastWriter().write(taskJson(it->second));
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
            line.pop_back();
        ofs << line << "\n";
    }
}

// ---------------------------------------------------------------------------
// 曾经连过的 agent: agents.json(整文件重写, 数量很小)
// ---------------------------------------------------------------------------
void JZAgentCliServer::loadAgentStore()
{
    m_agentFile = JZPlatform::applicationDirPath() + "/data/agents.json";

    ifstream ifs(m_agentFile, ios::binary);
    if (!ifs)
        return;

    stringstream ss;
    ss << ifs.rdbuf();
    Json::Value root;
    if (!parseJsonText(ss.str(), root) || !root.isArray())
    {
        jzWarning() << "agents.json 解析失败, 先忽略:" << m_agentFile;
        return;
    }

    for (Json::ArrayIndex i = 0; i < root.size(); ++i)
    {
        const Json::Value &j = root[i];
        KnownAgent a;
        a.agentId = j.get("agentId", "").asString();
        if (a.agentId.empty())
            continue;
        a.name = j.get("name", "").asString();
        a.version = j.get("version", "").asString();
        a.rootDir = j.get("rootDir", "").asString();
        a.modelName = j.get("modelName", "").asString();
        a.firstSeen = (long long)j.get("firstSeen", 0).asInt64();
        a.lastSeen = (long long)j.get("lastSeen", 0).asInt64();
        a.connects = j.get("connects", 0).asInt();
        m_known[a.agentId] = a;
    }
    jzInfo().noquote().nospace() << "agent 记录: 载入 " << m_known.size() << " 台(含离线)";
}

void JZAgentCliServer::saveAgentStore()
{
    if (m_agentFile.empty())
        return;
    Json::Value arr(Json::arrayValue);
    for (const auto &kv : m_known)
    {
        const KnownAgent &a = kv.second;
        Json::Value j(Json::objectValue);
        j["agentId"] = a.agentId;
        j["name"] = a.name;
        j["version"] = a.version;
        j["rootDir"] = a.rootDir;
        j["modelName"] = a.modelName;
        j["firstSeen"] = (Json::Int64)a.firstSeen;
        j["lastSeen"] = (Json::Int64)a.lastSeen;
        j["connects"] = a.connects;
        arr.append(j);
    }
    ofstream ofs(m_agentFile, ios::binary | ios::trunc);
    if (!ofs)
    {
        jzWarning() << "agent 记录写不进去:" << m_agentFile;
        return;
    }
    ofs << arr.toStyledString();
}

void JZAgentCliServer::upsertKnown(const KnownAgent &info)
{
    auto it = m_known.find(info.agentId);
    if (it == m_known.end())
    {
        KnownAgent a = info;
        if (a.firstSeen == 0)
            a.firstSeen = a.lastSeen;
        a.connects = 1;
        m_known[a.agentId] = a;
    }
    else
    {
        KnownAgent &a = it->second;
        if (a.firstSeen == 0)
            a.firstSeen = info.firstSeen ? info.firstSeen : a.lastSeen;
        if (!info.name.empty())
            a.name = info.name;
        if (!info.version.empty())
            a.version = info.version;
        if (!info.rootDir.empty())
            a.rootDir = info.rootDir;
        if (!info.modelName.empty())
            a.modelName = info.modelName;
        a.lastSeen = info.lastSeen;
        a.connects = a.connects + 1;
    }
    saveAgentStore();
}

// 列表页要的: 全部见过的 agent, 在线/离线都在里面(在线优先, 其次按最近一次在线排序)
Json::Value JZAgentCliServer::knownAgentsJson() const
{
    // Json::Value 的迭代器不是随机访问的, 不能直接 std::sort -> 先攒进 vector
    std::vector<Json::Value> items;
    items.reserve(m_known.size() + 1);

    for (const auto &kv : m_known)
    {
        const KnownAgent &a = kv.second;
        Json::Value j(Json::objectValue);
        j["agentId"] = a.agentId;
        j["name"] = a.name;
        j["version"] = a.version;
        j["rootDir"] = a.rootDir;
        j["modelName"] = a.modelName;
        j["firstSeen"] = (Json::Int64)a.firstSeen;
        j["connects"] = a.connects;
        j["builtin"] = false;

        auto ci = m_connByAgent.find(a.agentId);
        const bool online = (ci != m_connByAgent.end());
        j["online"] = online;
        j["lastSeen"] = (Json::Int64)a.lastSeen;
        j["busy"] = false;
        j["taskId"] = "";
        j["round"] = 0;
        j["lastEventType"] = "";
        j["nodes"] = Json::Value(Json::arrayValue);

        if (online)
        {
            auto oi = m_onlineByConn.find(ci->second);
            if (oi != m_onlineByConn.end())
            {
                const JZCliServerOnlineAgent &o = oi->second;
                j["busy"] = o.busy;
                j["taskId"] = o.taskId;
                j["round"] = o.round;
                j["lastSeen"] = (Json::Int64)o.lastSeen;
                j["lastEventType"] = o.lastEventType;
                Json::Value nodes;
                if (parseJsonText(o.nodes, nodes))
                    j["nodes"] = nodes;
            }
        }
        items.push_back(j);
    }

    // 内置 agent 永远在线, 也放进同一张表
    if (m_selfAgent)
    {
        Json::Value j(Json::objectValue);
        j["agentId"] = m_config.selfAgent.agentId;
        j["name"] = m_config.selfAgent.name;
        j["version"] = "builtin";
        j["rootDir"] = m_selfAgent->config().rootDir;
        j["modelName"] = m_config.selfAgent.modelName;
        j["firstSeen"] = (Json::Int64)m_startTime;
        j["connects"] = 1;
        j["builtin"] = true;
        j["online"] = true;
        j["busy"] = m_selfAgent->busy();
        j["taskId"] = m_selfAgent->taskId();
        j["round"] = m_selfAgent->round();
        j["lastSeen"] = (Json::Int64)JZDateTime::currentMSecsSinceEpoch();
        j["lastEventType"] = m_selfAgent->lastEventType();
        j["nodes"] = Json::Value(Json::arrayValue);
        items.push_back(j);
    }

    std::sort(items.begin(), items.end(), [](const Json::Value &x, const Json::Value &y)
    {
        const bool xo = x.get("online", false).asBool();
        const bool yo = y.get("online", false).asBool();
        if (xo != yo)
            return xo;   // 在线的排前面
        return x.get("lastSeen", (Json::Int64)0).asInt64() > y.get("lastSeen", (Json::Int64)0).asInt64();
    });

    Json::Value arr(Json::arrayValue);
    for (const auto &v : items)
        arr.append(v);
    return arr;
}

void JZAgentCliServer::recordCreate(const string &taskId, const string &agentId, const Json::Value &body)
{
    TaskRecord r;
    r.taskId = taskId;
    r.agentId = agentId;
    r.prompt = messageText(body);
    r.verifyCmd = body.get("verifyCmd", "").asString();
    r.state = kStateSubmitted;
    r.contextId = body.get("contextId", "").asString();
    if (r.contextId.empty())
        r.contextId = "ctx-" + taskId;   // 每条指令默认自成一个上下文; 前端可显式沿用
    r.startMs = r.statusMs = (long long)JZDateTime::currentMSecsSinceEpoch();

    // 请求里的 A2A Message 原样进 Task.history(补上 id / role / contextId)
    Json::Value msg = body.get("message", Json::Value(Json::objectValue));
    if (!msg.isObject())
        msg = Json::Value(Json::objectValue);
    msg["messageId"] = taskId + ":user";
    msg["role"] = "ROLE_USER";
    msg["taskId"] = taskId;
    msg["contextId"] = r.contextId;
    if (!msg.isMember("parts"))
    {
        Json::Value parts(Json::arrayValue);
        Json::Value part(Json::objectValue);
        part["text"] = r.prompt;
        parts.append(part);
        msg["parts"] = parts;
    }
    r.history.push_back(toJsonLine(msg));
    Json::Value submitted(Json::objectValue);
    submitted["agentId"] = agentId;
    submitted["state"] = kStateSubmitted;
    appendTaskEvent(&r, "submitted", submitted);

    if (m_tasks.find(taskId) == m_tasks.end())
        m_taskOrder.push_back(taskId);
    m_tasks[taskId] = r;
    while (m_taskOrder.size() > 500)
    {
        m_tasks.erase(m_taskOrder.front());
        m_taskOrder.erase(m_taskOrder.begin());
    }
    saveTaskRecord(r);
}

void JZAgentCliServer::recordEnd(const string &taskId, const Json::Value &result)
{
    TaskRecord *r = findTask(taskId);
    if (!r || !isWorkingState(r->state))
        return;   // timeout/lost 等 server 终态不能被迟到的 taskEnd 覆盖
    const string reply = result.get("agent_reply", "").asString();
    r->state = stateFromResult(result.get("status", "").asString());
    r->rounds = result.get("verify_rounds", 0).asInt();
    r->error = result.get("error", "").asString();
    r->endMs = r->statusMs = (long long)JZDateTime::currentMSecsSinceEpoch();

    // A2A: 结果放 Artifact, 不要用 Message 传输出
    if (!reply.empty())
    {
        Json::Value a(Json::objectValue);
        a["artifactId"] = "reply";
        a["name"] = "agent 回复";
        Json::Value parts(Json::arrayValue);
        Json::Value part(Json::objectValue);
        part["text"] = reply;
        parts.append(part);
        a["parts"] = parts;
        r->artifacts.push_back(toJsonLine(a));
    }
    const string verifyOut = result.get("last_verify_output", "").asString();
    if (!verifyOut.empty())
    {
        Json::Value a(Json::objectValue);
        a["artifactId"] = "verify";
        a["name"] = "验证输出";
        Json::Value parts(Json::arrayValue);
        Json::Value part(Json::objectValue);
        part["text"] = verifyOut;
        parts.append(part);
        a["parts"] = parts;
        r->artifacts.push_back(toJsonLine(a));
    }

    Json::Value tail(Json::objectValue);
    tail["state"] = r->state;
    tail["error"] = r->error;
    tail["verifyRounds"] = r->rounds;
    tail["artifacts"] = (int)r->artifacts.size();
    appendTaskEvent(r, "task_end", tail);
    saveTaskRecord(*r);

    broadcastTaskEvent(taskId, "artifact_update",
                       toJsonLine(jsonStringArray(r->artifacts)));
    finishTaskStreams(taskId);   // 终态: 补完整 Task 快照后关流
}

void JZAgentCliServer::recordFail(const string &taskId, const string &state, const string &error)
{
    TaskRecord *r = findTask(taskId);
    if (!r || !isWorkingState(r->state))
        return;
    r->state = state;
    r->error = error;
    r->endMs = r->statusMs = (long long)JZDateTime::currentMSecsSinceEpoch();
    Json::Value tail(Json::objectValue);
    tail["state"] = state;
    tail["error"] = error;
    appendTaskEvent(r, "task_end", tail);
    saveTaskRecord(*r);
    finishTaskStreams(taskId);
}

void JZAgentCliServer::appendTaskEvent(TaskRecord *r, const string &type, const Json::Value &data)
{
    if (!r)
        return;
    Json::Value e(Json::objectValue);
    e["seq"] = ++r->eventSeq;
    e["at"] = (Json::Int64)JZDateTime::currentMSecsSinceEpoch();
    e["type"] = type;
    e["data"] = data;
    r->events.push_back(toJsonLine(e));
    if (r->events.size() > 200)          // 有界: 一条巨型任务不会撑爆 tasks.jsonl
        r->events.erase(r->events.begin());
    r->lastEventType = type;
    r->statusMs = (long long)JZDateTime::currentMSecsSinceEpoch();
}

void JZAgentCliServer::recordEvent(const string &taskId, const string &type, const string &data)
{
    TaskRecord *r = findTask(taskId);
    if (!r || !isWorkingState(r->state))
        return;
    if (r->state == kStateSubmitted && type != "submitted")
        r->state = kStateWorking;       // 第一个执行事件: submitted -> working
    appendTaskEvent(r, type, looseJson(data));
    broadcastTaskEvent(taskId, "status_update", r->events.back());
    // 逐条落盘会把 JSONL 撑成 O(n^2); 每 20 条 + 终态各存一次, 中途 crash 只丢尾部过程。
    if (r->eventSeq % 20 == 0)
        saveTaskRecord(*r);
}

// ---- SSE 订阅者 ----
void JZAgentCliServer::removeTaskStream(JZHttpServerClient *client)
{
    for (auto it = m_taskStreams.begin(); it != m_taskStreams.end(); )
    {
        vector<JZHttpServerClient*> &v = it->second;
        v.erase(std::remove(v.begin(), v.end(), client), v.end());
        if (v.empty())
            it = m_taskStreams.erase(it);
        else
            ++it;
    }
}

void JZAgentCliServer::broadcastTaskEvent(const string &taskId, const string &event,
                                          const string &payload)
{
    auto it = m_taskStreams.find(taskId);
    if (it == m_taskStreams.end())
        return;
    const string chunk = sseChunk(makeSseFrame(event, payload));
    const vector<JZHttpServerClient*> clients = it->second;   // 拷贝: 回调可能改动订阅表
    for (JZHttpServerClient *c : clients)
        c->write(chunk.data(), (int)chunk.size(), false);
}

void JZAgentCliServer::finishTaskStreams(const string &taskId)
{
    auto it = m_taskStreams.find(taskId);
    if (it == m_taskStreams.end())
        return;
    TaskRecord *r = findTask(taskId);
    const string chunk = sseChunk(makeSseFrame("task",
        r ? toJsonLine(taskJson(*r)) : string("{}")));
    const vector<JZHttpServerClient*> clients = it->second;
    for (JZHttpServerClient *c : clients)
    {
        c->write(chunk.data(), (int)chunk.size(), false);
        c->write("0\r\n\r\n", 5, true);   // 终态关流; 前端 EventSource 也要主动 close
    }
    m_taskStreams.erase(it);
}

JZAgentCliServer::TaskRecord *JZAgentCliServer::findTask(const string &taskId)
{
    auto it = m_tasks.find(taskId);
    return (it == m_tasks.end()) ? nullptr : &it->second;
}

// 对外只有这一种形状: A2A v1.0 Task。管理所需的额外信息一律放 metadata。
Json::Value JZAgentCliServer::taskJson(const TaskRecord &r) const
{
    Json::Value j(Json::objectValue);
    j["id"] = r.taskId;
    j["contextId"] = r.contextId;

    Json::Value st(Json::objectValue);
    st["state"] = r.state;
    if (!r.error.empty())
    {
        Json::Value msg(Json::objectValue);
        msg["messageId"] = r.taskId + ":status";
        msg["role"] = "ROLE_AGENT";
        Json::Value parts(Json::arrayValue);
        Json::Value part(Json::objectValue);
        part["text"] = r.error;
        parts.append(part);
        msg["parts"] = parts;
        st["message"] = msg;
    }
    st["timestamp"] = isoTime(r.statusMs ? r.statusMs : (r.endMs ? r.endMs : r.startMs));
    j["status"] = st;
    j["artifacts"] = jsonStringArray(r.artifacts);
    j["history"] = jsonStringArray(r.history);

    Json::Value meta(Json::objectValue);
    meta["agentId"] = r.agentId;
    meta["prompt"] = r.prompt;
    meta["verifyCmd"] = r.verifyCmd;
    meta["rounds"] = r.rounds;
    meta["round"] = r.round;
    meta["lastEventType"] = r.lastEventType;
    meta["events"] = jsonStringArray(r.events);
    meta["eventSeq"] = r.eventSeq;
    meta["startMs"] = (Json::Int64)r.startMs;
    meta["endMs"] = (Json::Int64)r.endMs;
    meta["costMs"] = (Json::Int64)(r.endMs > 0 && r.startMs > 0 ? r.endMs - r.startMs : 0);
    meta["statusMs"] = (Json::Int64)(r.statusMs ? r.statusMs : (r.endMs ? r.endMs : r.startMs));
    j["metadata"] = meta;
    return j;
}

Json::Value JZAgentCliServer::tasksJson(const string &agentId, const string &state, int limit,
                                        const string &contextId) const
{
    Json::Value arr(Json::arrayValue);
    // 倒序(最新在前)
    int count = 0;
    for (auto it = m_taskOrder.rbegin(); it != m_taskOrder.rend() && count < limit; ++it)
    {
        auto ti = m_tasks.find(*it);
        if (ti == m_tasks.end())
            continue;
        if (!agentId.empty() && ti->second.agentId != agentId)
            continue;
        if (!state.empty() && ti->second.state != state)
            continue;
        if (!contextId.empty() && ti->second.contextId != contextId)
            continue;
        arr.append(taskJson(ti->second));
        ++count;
    }
    return arr;
}

void JZAgentCliServer::onApiTaskList(JZHttpServerClient *client, const JZHttpRequest &req)
{
    const string agentId = req.queryParam("agentId");
    const string state = req.queryParam("state");
    const string contextId = req.queryParam("contextId");
    int limit = atoi(req.queryParam("limit").c_str());
    if (limit <= 0 || limit > 500)
        limit = 50;

    Json::Value j(Json::objectValue);
    j["tasks"] = tasksJson(agentId, state, limit, contextId);
    j["nextPageToken"] = "";
    j["pageSize"] = limit;
    j["totalSize"] = (int)m_tasks.size();
    respondJson(client, req, 200, j);
}

void JZAgentCliServer::onApiTaskDetail(JZHttpServerClient *client, const JZHttpRequest &req)
{
    TaskRecord *r = findTask(req.queryParam("id"));
    if (!r)
    {
        Json::Value e(Json::objectValue);
        e["error"] = "没有这个任务";
        respondJson(client, req, 404, e);
        return;
    }
    respondJson(client, req, 200, taskJson(*r));
}

// A2A SubscribeToTask: 首帧必发当前 Task 快照, 之后逐帧推状态/产物事件, 终态关流。
// 传输用 chunked(与 agent_cli 的 /events 同口径): 不带 Content-Length 的流式响应在非浏览器
// 客户端里要等连接关闭才一次性交付, 那样就看不到过程了。
void JZAgentCliServer::onApiTaskSubscribe(JZHttpServerClient *client, const JZHttpRequest &req)
{
    const string taskId = req.queryParam("id");
    if (taskId.empty())
    {
        respondJson(client, req, 400, Json::Value("缺少 id"));
        return;
    }
    TaskRecord *r = findTask(taskId);
    if (!r)
    {
        respondJson(client, req, 404, Json::Value("没有这个任务: " + taskId));
        return;
    }

    JZHttpRespone resp;
    resp.setStatusCode(200);
    resp.setContentType("text/event-stream");
    resp.setHeader("Cache-Control", "no-cache");
    resp.setHeader("Connection", "keep-alive");
    resp.setHeader("X-Accel-Buffering", "no");
    resp.setHeader("Transfer-Encoding", "chunked");
    client->write(resp.toHeadByteArray(0, false), false);

    m_taskStreams[taskId].push_back(client);
    connect(static_cast<JZSignal<JZObject*>*>(&client->sigDestroyed), this,
            [this, client](JZObject *) { removeTaskStream(client); });

    const string first = sseChunk(makeSseFrame("task", toJsonLine(taskJson(*r))));
    client->write(first.data(), (int)first.size(), false);

    // 订阅的那一刻已经终态: 快照给完就关, 不留悬挂连接
    if (!isWorkingState(r->state))
    {
        client->write("0\r\n\r\n", 5, true);
        removeTaskStream(client);
    }
}

// A2A AgentCard: 管理台首页/详情页回答"这台 agent 是什么、能干什么"。
// 与 hello 上报是同一份数据, 只是换成标准形状; 在线状态由 server 的注册表补全。
Json::Value JZAgentCliServer::agentCardJson(const string &id) const
{
    Json::Value card(Json::objectValue);
    string name = id, version = "?", rootDir, modelName, description = "JZAgent 远端执行体";
    bool online = false, builtin = false, busy = false;
    long long firstSeen = 0, lastSeen = 0;
    int connects = 0, round = 0;
    string taskId, nodesText;

    auto ki = m_known.find(id);
    if (ki != m_known.end())
    {
        const KnownAgent &a = ki->second;
        if (!a.name.empty()) name = a.name;
        version = a.version;
        rootDir = a.rootDir;
        modelName = a.modelName;
        firstSeen = a.firstSeen;
        lastSeen = a.lastSeen;
        connects = a.connects;
    }
    auto ci = m_connByAgent.find(id);
    if (ci != m_connByAgent.end())
    {
        online = true;
        auto oi = m_onlineByConn.find(ci->second);
        if (oi != m_onlineByConn.end())
        {
            const JZCliServerOnlineAgent &o = oi->second;
            busy = o.busy;
            taskId = o.taskId;
            round = o.round;
            nodesText = o.nodes;
            lastSeen = o.lastSeen;
            if (!o.modelName.empty()) modelName = o.modelName;
            if (!o.rootDir.empty()) rootDir = o.rootDir;
        }
    }
    if (m_selfAgent && isSelfAgentId(id))
    {
        builtin = true;
        online = true;
        busy = m_selfAgent->busy();
        taskId = m_selfAgent->taskId();
        round = m_selfAgent->round();
        version = "builtin";
        rootDir = m_selfAgent->config().rootDir;
        modelName = m_config.selfAgent.modelName;
        description = "server 进程内置 agent(可管理集群)";
        lastSeen = JZDateTime::currentMSecsSinceEpoch();
    }

    card["name"] = name;
    card["description"] = description;
    card["version"] = version.empty() ? string("?") : version;
    Json::Value iface(Json::objectValue);
    iface["url"] = "jznet://hub/" + id;      // 我们走自定义 JZNet 绑定, 不是 HTTP+JSON
    iface["protocolBinding"] = "JZNET_CUSTOM";
    iface["protocolVersion"] = "1.0";
    Json::Value ifaces(Json::arrayValue);
    ifaces.append(iface);
    card["supportedInterfaces"] = ifaces;
    card["defaultInputModes"] = Json::Value(Json::arrayValue);
    card["defaultInputModes"].append("text/plain");
    card["defaultOutputModes"] = Json::Value(Json::arrayValue);
    card["defaultOutputModes"].append("text/plain");

    card["capabilities"]["streaming"] = true;       // /api/tasks/subscribe
    card["capabilities"]["pushNotifications"] = false;
    card["capabilities"]["extendedAgentCard"] = false;

    Json::Value skills(Json::arrayValue);
    Json::Value s(Json::objectValue);
    s["id"] = "task";
    s["name"] = "执行任务";
    s["description"] = "接受一条自然语言指令, 在本机工作区里用工具执行并回报结果";
    s["tags"] = Json::Value(Json::arrayValue);
    s["tags"].append("task");
    s["tags"].append(builtin ? "builtin" : "remote");
    skills.append(s);
    Json::Value nodes(Json::arrayValue);
    parseJsonText(nodesText, nodes);
    if (nodes.isArray())
    {
        for (Json::ArrayIndex i = 0; i < nodes.size(); ++i)
        {
            Json::Value n(Json::objectValue);
            n["id"] = "node:" + nodes[i].get("name", "").asString();
            n["name"] = nodes[i].get("name", "node").asString();
            n["description"] = "远端执行节点: " + nodes[i].get("target", "").asString();
            n["tags"] = Json::Value(Json::arrayValue);
            n["tags"].append("node");
            skills.append(n);
        }
    }
    card["skills"] = skills;
    card["securitySchemes"]["mtls"]["type"] = "mutualTLS";

    Json::Value meta(Json::objectValue);
    meta["agentId"] = id;
    meta["online"] = online;
    meta["builtin"] = builtin;
    meta["busy"] = busy;
    meta["taskId"] = taskId;
    meta["round"] = round;
    meta["rootDir"] = rootDir;
    meta["modelName"] = modelName;
    meta["firstSeen"] = (Json::Int64)firstSeen;
    meta["lastSeen"] = (Json::Int64)lastSeen;
    meta["connects"] = connects;
    meta["nodes"] = nodes.isArray() ? nodes : Json::Value(Json::arrayValue);
    card["metadata"] = meta;
    return card;
}

void JZAgentCliServer::onApiAgentCard(JZHttpServerClient *client, const JZHttpRequest &req)
{
    const string id = req.queryParam("id");
    if (id.empty())
    {
        respondJson(client, req, 400, Json::Value("缺少 id"));
        return;
    }
    const bool exists = m_known.find(id) != m_known.end() ||
                        m_connByAgent.find(id) != m_connByAgent.end() ||
                        m_agents.find(id) != m_agents.end() ||
                        (m_selfAgent && isSelfAgentId(id));
    if (!exists)
    {
        respondJson(client, req, 404, Json::Value("没有这个 agent: " + id));
        return;
    }
    respondJson(client, req, 200, agentCardJson(id));
}

Json::Value JZAgentCliServer::snapshotTask(const string &taskId) const
{
    auto it = m_tasks.find(taskId);
    if (it == m_tasks.end())
        return Json::Value(Json::nullValue);
    return taskJson(it->second);
}

// 单个 agent 的详情: 内置 agent / 在线 cli / 本机子进程, 三选一
void JZAgentCliServer::onApiAgentDetail(JZHttpServerClient *client, const JZHttpRequest &req)
{

    const string id = req.queryParam("id");
    if (id.empty())
    {
        respondJson(client, req, 400, Json::Value("缺少 id"));
        return;
    }

    Json::Value out(Json::objectValue);
    Json::Value all = onlineAgentsJson();
    for (Json::ArrayIndex i = 0; i < all.size(); ++i)
    {
        if (all[i].get("agentId", "").asString() != id)
            continue;
        out["kind"] = all[i].get("builtin", false).asBool() ? "builtin" : "hub";
        out["info"] = all[i];
        break;
    }

    // 离线但有档案: 仍然给详情(列表页允许点进一台已经掉线的 agent)
    if (!out.isMember("info"))
    {
        auto ki = m_known.find(id);
        if (ki != m_known.end())
        {
            const KnownAgent &a = ki->second;
            Json::Value j(Json::objectValue);
            j["agentId"] = a.agentId;
            j["name"] = a.name;
            j["version"] = a.version;
            j["rootDir"] = a.rootDir;
            j["modelName"] = a.modelName;
            j["online"] = false;
            j["busy"] = false;
            j["taskId"] = "";
            j["round"] = 0;
            j["lastSeen"] = (Json::Int64)a.lastSeen;
            j["firstSeen"] = (Json::Int64)a.firstSeen;
            j["connects"] = a.connects;
            j["lastEventType"] = "";
            j["nodes"] = Json::Value(Json::arrayValue);
            out["kind"] = "offline";
            out["info"] = j;
        }
    }

    if (!out.isMember("info"))
    {
        auto it = m_agents.find(id);
        if (it == m_agents.end())
        {
            Json::Value e(Json::objectValue);
            e["error"] = "没有这个 agent: " + id + "(既不在线, 也不是本地子进程)";
            Json::Value ids(Json::arrayValue);
            for (Json::ArrayIndex i = 0; i < all.size(); ++i)
                ids.append(all[i].get("agentId", ""));
            for (const auto &kv : m_agents)
                ids.append(kv.first);
            e["known"] = ids;
            respondJson(client, req, 404, e);
            return;
        }
        JZCliServerAgentInstance *inst = it->second;
        Json::Value j(Json::objectValue);
        j["agentId"] = id;
        j["name"] = id;
        j["kind"] = "process";
        j["state"] = (int)inst->state;
        j["startTime"] = (Json::Int64)inst->startTime;
        j["exitCode"] = inst->exitCode;
        j["cmd"] = inst->cfg.cmd;
        j["nodes"] = Json::Value(Json::arrayValue);
        out["kind"] = "process";
        out["info"] = j;
    }

    out["tasks"] = tasksJson(id, "", 20);
    out["card"] = agentCardJson(id);      // A2A: 能力自述, 详情页直接渲染
    respondJson(client, req, 200, out);
}

// 删除一台 agent 的档案(只删"曾经连过"的记录; 默认保留历史任务)
void JZAgentCliServer::onApiAgentForget(JZHttpServerClient *client, const JZHttpRequest &req)
{

    string id = req.queryParam("agentId");
    if (id.empty())
        id = req.queryParam("id");
    if (id.empty())
    {
        respondJson(client, req, 400, Json::Value("缺少 agentId"));
        return;
    }
    if (isSelfAgentId(id))
    {
        respondJson(client, req, 400,
                    Json::Value("内置 agent 不能删除(它跟着 selfAgent.enabled 一起存在)"));
        return;
    }
    if (m_connByAgent.find(id) != m_connByAgent.end())
    {
        // 在线就删会导致"列表没了但连接还在", 下一次心跳又冒出来
        Json::Value e(Json::objectValue);
        e["error"] = "这台 agent 还在线, 先停掉它(或等它掉线)再删";
        e["online"] = true;
        respondJson(client, req, 409, e);
        return;
    }

    auto it = m_known.find(id);
    if (it == m_known.end())
    {
        Json::Value e(Json::objectValue);
        e["error"] = "列表里没有这台 agent";
        respondJson(client, req, 404, e);
        return;
    }
    m_known.erase(it);
    saveAgentStore();

    int removedTasks = 0;
    if (req.queryParam("withTasks") == "1")
    {
        for (auto tit = m_tasks.begin(); tit != m_tasks.end(); )
        {
            if (tit->second.agentId == id)
            {
                tit = m_tasks.erase(tit);
                ++removedTasks;
            }
            else
            {
                ++tit;
            }
        }
        vector<string> keep;
        keep.reserve(m_taskOrder.size());
        for (const auto &tid : m_taskOrder)
            if (m_tasks.find(tid) != m_tasks.end())
                keep.push_back(tid);
        m_taskOrder.swap(keep);
        rewriteTaskStore();   // 追加式文件不重写的话, 重启会把删掉的行"复活"
    }

    Json::Value j(Json::objectValue);
    j["ok"] = true;
    j["agentId"] = id;
    j["removedTasks"] = removedTasks;
    j["note"] = removedTasks ? "已删除 agent 及其任务记录"
                             : "只从列表移除; 任务记录保留(加 withTasks=1 可一并删除)";
    jzInfo().noquote().nospace() << "agent 档案删除: " << id << "  tasks=" << removedTasks;
    respondJson(client, req, 200, j);
}

void JZAgentCliServer::servePage(JZHttpServerClient *client, const JZHttpRequest &req,
                                 const std::string &name)
{
    const string path = dashboardDir() + "/" + name;
    ifstream ifs(path, ios::binary);
    if (!ifs)
    {
        respondText(client, req, 404,
                    "页面不存在: " + path + " (发布时要带上 server_web/)", "text/plain; charset=utf-8");
        return;
    }
    stringstream ss;
    ss << ifs.rdbuf();
    respondText(client, req, 200, ss.str(), "text/html; charset=utf-8");
}

// ---------------------------------------------------------------------------
// 任务下发: 核心(HTTP 与 server 内置 agent 的工具共用)
//   返回 200 = 已登记同步等待(调用方**不要**再回包, 回包在 taskEnd/done 时发出)
//   返回 202 = 已接受(异步)
//   返回 >=400 = 失败(ack 里有 error, 调用方直接回包)
// ---------------------------------------------------------------------------
bool JZAgentCliServer::isSelfAgentId(const string &id) const
{
    return m_selfAgent && !m_config.selfAgent.agentId.empty() && id == m_config.selfAgent.agentId;
}

int JZAgentCliServer::submitTask(const Json::Value &body, JZHttpServerClient *waiter,
                                 Json::Value &ack, string &error)
{
    // A2A SendMessage 是唯一入口。cli 的 /task 载荷在边界转换一次, 对外不再出现 prompt/verify_cmd。
    const string prompt = messageText(body);
    if (prompt.empty())
    {
        error = "缺少 message.parts[].text";
        ack["error"] = error;
        return 400;
    }
    string agentId = body.get("agentId", "").asString();
    if (agentId.empty() && m_onlineByConn.size() == 1 && !m_selfAgent)
        agentId = m_onlineByConn.begin()->second.agentId;   // 只有一台就默认它
    if (agentId.empty())
    {
        error = "缺少 agentId(当前在线 " + to_string(m_onlineByConn.size()) + " 台"
                + (m_selfAgent ? ", 另有内置 server agent" : "") + ")";
        ack["error"] = error;
        return 400;
    }

    Json::Value cliBody(Json::objectValue);
    cliBody["prompt"] = prompt;
    cliBody["verify_cmd"] = body.get("verifyCmd", "").asString();
    cliBody["use_memory"] = false;
    if (body.isMember("verifyTimeout"))   cliBody["verify_timeout"] = body["verifyTimeout"];
    if (body.isMember("maxVerifyRounds")) cliBody["max_verify_rounds"] = body["maxVerifyRounds"];
    if (body.isMember("maxRetries"))      cliBody["max_retries"] = body["maxRetries"];
    if (body.isMember("maxRounds"))       cliBody["max_rounds"] = body["maxRounds"];

    const string taskId = "t" + to_string(JZDateTime::currentMSecsSinceEpoch()) + "-" + to_string(++m_taskSeq);

    int timeoutMs = body.get("taskTimeoutMs", 900000).asInt();
    if (timeoutMs < 1000) timeoutMs = 1000;
    if (timeoutMs > 24 * 3600 * 1000) timeoutMs = 24 * 3600 * 1000;

    // 所有在跑任务都登记超时; waiter 只是可选回包通道, 不能决定任务能否被取消/超时。
    JZTimer *timer = new JZTimer(this);
    timer->setSingleShot(true);
    connect(&timer->sigTimeout, this, [this, taskId]()
    {
        auto pit = m_pendingTasks.find(taskId);
        if (pit == m_pendingTasks.end())
            return;
        PendingTask pt2 = pit->second;
        m_pendingTasks.erase(pit);

        // 先落终态再取消: selfAgent.stop() 会同步发 done, 不能让这次 done 抢先写成 canceled。
        recordFail(taskId, kStateFailed, "任务超时(server 侧)");
        if (isSelfAgentId(pt2.agentId))
        {
            if (m_selfAgent)
                m_selfAgent->cancel();
        }
        else
        {
            auto ai2 = m_connByAgent.find(pt2.agentId);
            if (ai2 != m_connByAgent.end() && m_hub)
            {
                JZHubPackTaskCancel cancel;
                cancel.taskId = taskId;
                m_hub->sendPacket(ai2->second, &cancel);
            }
        }
        if (pt2.timer)
            pt2.timer->deleteLater();

        // 回包与 GetTask 同形状(504 只是 HTTP 状态, body 仍是 Task)
        TaskRecord *rec = findTask(taskId);
        respondJsonAsync(pt2.client, 504, rec ? taskJson(*rec) : Json::Value(Json::objectValue));
    });
    timer->start(timeoutMs);

    if (waiter)
    {
        // 提交方提前断开: 只丢回包通道, 任务照跑(cli 侧行为与 HTTP /task 一致)
        connect(static_cast<JZSignal<JZObject*>*>(&waiter->sigDestroyed), this, [this](JZObject *obj)
        {
            for (auto &kv : m_pendingTasks)
                if (kv.second.client == static_cast<JZHttpServerClient *>(obj))
                    kv.second.client = nullptr;
        });
    }

    // ---- 内置 agent: 任务在 server 进程内跑 ----
    if (isSelfAgentId(agentId))
    {
        if (m_selfAgent->busy())
        {
            if (timer) { timer->stop(); timer->deleteLater(); }
            error = "内置 agent 正忙: " + m_selfAgent->taskId();
            ack["error"] = error;
            ack["taskId"] = m_selfAgent->taskId();
            return 409;
        }

        recordCreate(taskId, agentId, body);
        PendingTask selfPending;
        selfPending.taskId = taskId;
        selfPending.agentId = agentId;
        selfPending.client = waiter;
        selfPending.startMs = JZDateTime::currentMSecsSinceEpoch();
        selfPending.timer = timer;
        m_pendingTasks[taskId] = selfPending;
        m_selfAgent->submit(taskId, prompt, body.get("verifyCmd", "").asString());
        ack = taskJson(*findTask(taskId));      // SendMessage 的回包就是一个 Task
        jzInfo().noquote().nospace() << "任务下发(内置): " << taskId;
        return waiter ? 200 : 202;
    }

    // ---- 远端 cli: 走控制面 ----
    auto ai = m_connByAgent.find(agentId);
    if (ai == m_connByAgent.end())
    {
        if (timer) { timer->stop(); timer->deleteLater(); }
        error = "没有在线的 agent: " + agentId;
        ack["error"] = error;
        Json::Value ids(Json::arrayValue);
        for (const auto &kv : m_connByAgent)
            ids.append(kv.first);
        ack["online"] = ids;
        return 404;
    }

    JZCliServerOnlineAgent &agent = m_onlineByConn[ai->second];
    // 注意: 这里**不再**因为 agent.busy 就 409 —— cli 侧有任务队列(task.queues),
    // 忙时它会自己排队, server 拦下来反而把"多队列"能力废掉了。

    recordCreate(taskId, agentId, body);   // 先落记录: 后面任何一条退出路径都能查到它

    JZHubPackTaskSubmit submit;
    submit.taskId = taskId;
    string payload = Json::FastWriter().write(cliBody);   // hub 下游是 cli 的 /task 载荷
    while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r'))
        payload.pop_back();
    submit.body = payload;

    PendingTask pending;
    pending.taskId = taskId;
    pending.agentId = agentId;
    pending.client = waiter;
    pending.startMs = JZDateTime::currentMSecsSinceEpoch();
    pending.timer = timer;
    m_pendingTasks[taskId] = pending;

    agent.busy = true;
    agent.taskId = taskId;
    agent.round = 0;

    m_hub->sendPacket(ai->second, &submit);
    jzInfo().noquote().nospace() << "任务下发: " << taskId << " -> " << agentId;

    ack = taskJson(*findTask(taskId));          // SendMessage 的回包就是一个 Task
    return waiter ? 200 : 202;
}

int JZAgentCliServer::submitTaskForTool(const Json::Value &body, Json::Value &ack, string &error)
{
    return submitTask(body, nullptr, ack, error);
}

// 内置 agent 的事件流: done = 收口(落记录 + 回等待方), 其余只更新"最近事件"
void JZAgentCliServer::onSelfAgentEvent(const JZServerAgentEvent &ev)
{
    if (ev.type == "done")
    {
        Json::Value result;
        if (!parseJsonText(ev.data, result) || !result.isObject())
        {
            result = Json::Value(Json::objectValue);
            result["status"] = "failed";
            result["error"] = "内置 agent 的结果不是合法 JSON";
        }
        result["taskId"] = Json::Value(ev.taskId);
        recordEnd(ev.taskId, result);

        auto pit = m_pendingTasks.find(ev.taskId);
        if (pit != m_pendingTasks.end())
        {
            PendingTask pt = pit->second;
            m_pendingTasks.erase(pit);
            if (pt.timer)
            {
                pt.timer->stop();
                pt.timer->deleteLater();
            }
            TaskRecord *rec = findTask(ev.taskId);
            respondJsonAsync(pt.client, 200, rec ? taskJson(*rec) : Json::Value(Json::objectValue));
        }
        TaskRecord *rec = findTask(ev.taskId);
        jzInfo().noquote().nospace() << "内置 agent 任务结束: " << ev.taskId
                                     << "  state=" << (rec ? rec->state : string(kStateUnknown));
        return;
    }
    recordEvent(ev.taskId, ev.type, ev.data);
}

// ---------------------------------------------------------------------------
// A2A SendMessage(POST /api/tasks): 默认阻塞到终态; returnImmediately=true 立即回 Task。
// ---------------------------------------------------------------------------
void JZAgentCliServer::onApiTasks(JZHttpServerClient *client, const JZHttpRequest &req)
{

    Json::Value body;
    if (!parseJsonText(string(req.body().data(), req.body().size()), body) || !body.isObject())
    {
        respondJson(client, req, 400, Json::Value("body 必须是 JSON 对象"));
        return;
    }

    JZHttpServerClient *waiter = body.get("returnImmediately", false).asBool() ? nullptr : client;

    Json::Value ack(Json::objectValue);
    string error;
    const int code = submitTask(body, waiter, ack, error);
    if (code == 200)
        return;   // 同步模式: 回包在 taskEnd / done 时发出
    respondJson(client, req, code, ack);
}

void JZAgentCliServer::onApiTaskCancel(JZHttpServerClient *client, const JZHttpRequest &req)
{

    // A2A CancelTask: 用 Task.id 指定任务(?id= 或 body {"id": "..."})
    string taskId = req.queryParam("id");
    if (taskId.empty())
    {
        Json::Value body;
        if (parseJsonText(string(req.body().data(), req.body().size()), body) && body.isObject())
            taskId = body.get("id", "").asString();
    }
    if (taskId.empty())
    {
        respondJson(client, req, 400, Json::Value("缺少 id"));
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

    if (isSelfAgentId(pit->second.agentId))
    {
        if (m_selfAgent)
            m_selfAgent->cancel();
    }
    else
    {
        auto ai = m_connByAgent.find(pit->second.agentId);
        if (ai != m_connByAgent.end() && m_hub)
        {
            JZHubPackTaskCancel cancel;
            cancel.taskId = taskId;
            m_hub->sendPacket(ai->second, &cancel);
        }
    }

    // A2A CancelTask: 返回更新后的 Task, 且**不保证真的取消成功**(可能刚好已跑完)。
    // 状态仍是非终态是对的, 前端要继续跟到终态, 不能自己把徽标改成 canceled。
    TaskRecord *rec = findTask(taskId);
    respondJson(client, req, 202, rec ? taskJson(*rec) : Json::Value(Json::objectValue));
}

// ---------------------------------------------------------------------------
// 后台网页 + 管理 API
// ---------------------------------------------------------------------------
void JZAgentCliServer::startHttp()
{
    m_http = new JZHttpServer(this);

    // HTTPS: 证书在 init() 里已读进内存(读不到就不会走到这里)。
    //   sslCa 非空 ⇒ mongoose 服务端用 SSL_VERIFY_PEER|FAIL_IF_NO_PEER_CERT, 即强制客户端证书;
    //   只给 cert/key 不给 CA ⇒ 单向 TLS(仅加密, 不验客户端)。
    if (m_config.http.sslEnabled)
    {
        JZSSLConfig ssl;
        ssl.sslCert = m_httpCertPem;
        ssl.sslKey = m_httpKeyPem;
        if (m_config.http.requireClientCert)
            ssl.sslCa = m_httpCaPem;
        m_http->setSslConfig(ssl);
    }
    else
    {
        jzWarning() << "http.ssl.enabled=false: 管理台/API 将以明文 HTTP 暴露(仅限完全可信的隔离网络)";
    }

    // 任务记录 + agent 档案: 先载盘, 否则重启后历史全丢
    loadTaskStore();
    loadAgentStore();

    // SSE 订阅保活: 长任务没有事件时, 代理/浏览器可能把空闲连接掐掉
    m_streamTimer = new JZTimer(this);
    connect(&m_streamTimer->sigTimeout, this, [this]()
    {
        if (m_taskStreams.empty())
            return;
        const string chunk = sseChunk(": keepalive\n\n");
        for (auto &kv : m_taskStreams)
            for (JZHttpServerClient *c : kv.second)
                c->write(chunk.data(), (int)chunk.size(), false);
    });
    m_streamTimer->start(15000);

    // 管理 API(精确路由, 优先于兜底)
    m_http->addRoute("/api/status", new ApiStatusHandler(this));
    m_http->addRoute("/api/tasks/cancel", new ApiTaskCancelHandler(this));
    m_http->addRoute("/api/tasks/detail", new ApiTaskDetailHandler(this));
    m_http->addRoute("/api/tasks/subscribe", new ApiTaskSubscribeHandler(this));
    m_http->addRoute("/api/tasks", new ApiTasksHandler(this));
    m_http->addRoute("/api/agent", new ApiAgentDetailHandler(this));
    m_http->addRoute("/api/agent/card", new ApiAgentCardHandler(this));
    m_http->addRoute("/api/agent/forget", new ApiAgentForgetHandler(this));
    m_http->addRoute("/api/agents", new ApiAgentsHandler(this));
    m_http->addRoute("/api/agents/logs", new ApiAgentLogsHandler(this));
    m_http->addRoute("/api/agents/start", new ApiAgentActionHandler(this, "start"));
    m_http->addRoute("/api/agents/stop", new ApiAgentActionHandler(this, "stop"));
    m_http->addRoute("/api/agents/restart", new ApiAgentActionHandler(this, "restart"));

    // 管理台页面资源: 全部来自 <exeDir>/server_web/ (CMake 从 server/web + web/ui 拷过来)
    //   /app.css /app.js   页面自己的样式与脚本
    //   /ui/*              共享视觉层(tokens/base/components/jz-ui.js)
    // 目录缺失时 `/` 会回 500 并说明"缺 server_web/", 不会静默给出空白页。
    JZHttpStaticFiles *webFiles = new JZHttpStaticFiles(dashboardDir());
    // 管理台的页面每次构建都可能变: 让浏览器回源校验(ETag), 不要吃 1 小时缓存
    webFiles->setCacheMaxAge(0);
    m_http->addRoute("/app.css", webFiles);
    m_http->addRoute("/app.js", webFiles);
    m_http->addRoute("/ui/<path:path>", webFiles);

    // 多页: 一个页面一个文件(连接列表 / agent 详情 / 任务列表 / 新建任务)
    m_http->addRoute("/agent", new PageHandler(this, "agent.html"));
    m_http->addRoute("/tasks", new PageHandler(this, "tasks.html"));
    m_http->addRoute("/new", new PageHandler(this, "new.html"));

    // 首页 + 兜底(未知路径也回首页, 方便深链)
    m_http->addRoute("/", new DashboardHandler(this));
    m_http->addRoute("/<path:path>", new DashboardHandler(this));

    if (!m_http->listen(m_config.http.bind, m_config.http.port))
        jzWarning() << "管理网页监听失败:" << m_config.http.bind << ":" << m_config.http.port;
    else
        jzInfo() << "管理网页已启动: " << (m_config.http.sslEnabled ? "https://" : "http://")
                 << m_config.http.bind << ":" << m_config.http.port
                 << (m_config.http.sslEnabled
                         ? (m_config.http.requireClientCert ? "  (需客户端证书)" : "  (单向 TLS)")
                         : "  (明文!)");
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
// 路由实现
// ---------------------------------------------------------------------------
void JZAgentCliServer::onApiStatus(JZHttpServerClient *client, const JZHttpRequest &req)
{
    Json::Value j;
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
    // 曾经连过的全部(含离线): 列表页要能回答"这台以前在不在"
    j["known"] = knownAgentsJson();
    respondJson(client, req, 200, j);
}

void JZAgentCliServer::onApiAgents(JZHttpServerClient *client, const JZHttpRequest &req)
{
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
    // 页面从磁盘读(web/ -> <exeDir>/server_web/), 这里只做读文件 + 回包。
    // 鉴权在传输层: 默认 HTTPS + 强制客户端证书(mTLS), 没有 token 这一层。
    string html;
    string loadError;
    if (!loadDashboardHtml(html, loadError))
    {
        respondText(client, req, 500, loadError, "text/plain; charset=utf-8");
        return;
    }
    respondText(client, req, 200, html, "text/html; charset=utf-8");
}

// ---------------------------------------------------------------------------
// 管理台网页: 从磁盘读, 不再内嵌 HTML/CSS/JS 字符串
//   web/(src) --CMake--> <exeDir>/server_web/ , 这里按 dashboardDir() 去读
//   好处: 改样式/脚本不用重编译; 现场能直接改文件排查(改坏了从包里重拷一份)。
// ---------------------------------------------------------------------------
string JZAgentCliServer::dashboardDir() const
{
    return JZPlatform::applicationDirPath() + "/server_web";
}

bool JZAgentCliServer::loadDashboardHtml(string &html, string &error) const
{
    const string path = dashboardDir() + "/index.html";
    ifstream ifs(path, ios::binary);
    if (!ifs)
    {
        error = "读不到管理台页面: " + path + " (发布时要把 server_web/ 目录放在 exe 同目录)";
        return false;
    }
    stringstream ss;
    ss << ifs.rdbuf();
    html = ss.str();
    if (html.empty())
    {
        error = "管理台页面是空文件: " + path;
        return false;
    }
    return true;
}

