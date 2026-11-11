#ifndef JZ_AGENT_CLI_SERVER_H_
#define JZ_AGENT_CLI_SERVER_H_

#include <map>
#include <string>
#include <vector>

#include "JZJson.h"
#include "JZNet/JZNetPacket.h"   // JZNetPackPtr(onHubPacket 的入参)
#include "JZObject.h"

class JZHttpServer;
class JZProcess;
#include "JZServerAgent.h"

class JZTimer;
class JZNetServer;
class JZHttpServerClient;
class JZHttpRequest;

// ---------------------------------------------------------------------------
// JZAgentCliServer —— agent_cli 管理服务器
//   1) 控制面(hub): 等 cli 主动连进来, 点名下发 A2A Task;
//   2) 管理一系列 JZAgentCli 实例: 每个作为一个子进程拉起并监管(启停/重启/状态);
//   3) 收集各 agent 的 stdout/stderr 日志, 提供后台网页 + 管理 API 查询。
//   网页/API 都在本类的 JZHttpServer 上; 端口由 config.http.* 控制。
//   内网穿透不在本进程里做: 需要时用独立的 jzfrps / jzfrpc 进程。
// ---------------------------------------------------------------------------

// 后台网页 / 管理 API 配置
//   HTTPS 默认开启, 且默认要求客户端证书(双向认证 / mTLS): 管理台能停/起 agent、
//   能往任意 agent 下发任务, 明文 HTTP 一旦被访问就等于把执行权交出去。
//   只有显式 ssl.enabled=false 才会退回明文(启动时打警告)。
struct JZCliServerHttpConfig
{
    std::string bind = "127.0.0.1";
    int port = 8088;

    // 鉴权只靠传输层: 默认 HTTPS + 强制客户端证书。不再有应用层 token。
    bool sslEnabled = true;     // 是否用 TLS 提供网页 / API(默认开)
    std::string certFile;       // 服务器证书链(PEM); 空 = <配置文件目录>/certs/server.crt
    std::string keyFile;        // 服务器私钥(PEM);   空 = <配置文件目录>/certs/server.key
    std::string caFile;         // 客户端 CA(PEM);    空 = <配置文件目录>/certs/ca.crt
    bool requireClientCert = true;  // 是否校验客户端证书(需要 caFile; false = 单向 TLS)
};

// 被管理的一个 agent_cli 实例的配置
struct JZCliServerAgentConfig
{
    std::string name;           // 唯一名(用于 API / 网页标识)
    bool enabled = true;        // 是否随 server 启动
    std::string cmd = "agent_cli";   // 程序; 默认解析到 server 同目录的 agent_cli(.exe)
    std::vector<std::string> args;   // 启动参数, 如 ["-c","agent1.json"]
    std::string dir;            // 工作目录(空 = server 同目录)
};

// 控制面(hub): cli 出站连进来的 JZNetServer
//   ⚠ 与 http.port(8088, 管理网页/API)是两个端口: 这是二进制协议, 只给 cli 用。
struct JZCliServerHubConfig
{
    std::string bind = "0.0.0.0";
    int port = 8090;
    std::string token;        // JZNet 握手口令; 空 = 不校验(仅限完全可信的网络)
};

// 已上线的一台 agent_cli(控制面连接)的视图
struct JZCliServerOnlineAgent
{
    uint64_t conn = 0;
    std::string agentId;
    std::string name;
    std::string version;
    std::string rootDir;
    std::string modelName;
    std::string nodes;         // 节点清单 JSON 数组字符串(cli 上报)
    bool busy = false;
    int round = 0;
    std::string taskId;
    int64_t lastSeen = 0;
    std::string lastEventType;
    std::string lastEventData;  // JSON(已按包上限裁过)
    bool acceptTasks = true;
};

// JZAgentCliServer 配置: 后台网页 + 被管理的一系列 agent_cli + 控制面
struct JZCliServerConfig
{
    // 后台网页 / 管理 API
    JZCliServerHttpConfig http;

    // 控制面: 等 agent_cli 出站连进来(替代 v1 的"拉本机子进程")
    JZCliServerHubConfig hub;

    // 管理的一系列 agent_cli(各作为一个子进程拉起并监管)
    std::vector<JZCliServerAgentConfig> agents;

    // server 自己那个 JZAgent(内置 agent, 跑在 server 进程内, agentId 默认 "server")
    JZServerAgentConfig selfAgent;
};

// 单个被管理 agent 的运行实例(实现细节, 见 cpp)
struct JZCliServerAgentInstance;

class JZAgentCliServer : public JZObject
{
public:
    explicit JZAgentCliServer(JZObject *parent = nullptr);
    ~JZAgentCliServer() override;

    bool init(const JZCliServerConfig &config, std::string &error);

    // 启动全部: 控制面 + 后台网页/管理 API + 拉起所有 enabled 的 agent。事件循环前调用。
    void start();

    // ---- 对外的 agent 管理(网页/API 调用) ----
    bool spawnAgent(const std::string &name);   // 启动(已在跑则忽略)
    bool stopAgent(const std::string &name);    // 优雅停止
    bool restartAgent(const std::string &name); // 重启

    // ---- 给 server 内置 agent 的工具用(实现见 JZServerAgent.cpp) ----
    Json::Value snapshotAgents() const { return onlineAgentsJson(); }
    Json::Value snapshotTasks(const std::string &agentId, const std::string &state, int limit) const
    {
        return tasksJson(agentId, state, limit);
    }
    Json::Value snapshotTask(const std::string &taskId) const;   // 不存在返回 null
    // 供工具调用: 与 POST /api/tasks 同一套逻辑, 但不等回包(waiter=nullptr)
    int submitTaskForTool(const Json::Value &body, Json::Value &ack, std::string &error);

    // 路由处理器(供本类内 handler 子类调用, 需 public)
    void onApiStatus(JZHttpServerClient *client, const JZHttpRequest &req);
    void onApiAgents(JZHttpServerClient *client, const JZHttpRequest &req);
    void onApiAgentLogs(JZHttpServerClient *client, const JZHttpRequest &req);
    void onApiAgentAction(JZHttpServerClient *client, const JZHttpRequest &req,
                          const std::string &action);  // start / stop / restart
    // 控制面: 提交/取消任务(body = POST /task 的字段 + agentId)
    void onApiTasks(JZHttpServerClient *client, const JZHttpRequest &req);
    void onApiTaskCancel(JZHttpServerClient *client, const JZHttpRequest &req);
    // 任务记录(server 侧历史, 落盘 tasks.jsonl)
    void onApiTaskList(JZHttpServerClient *client, const JZHttpRequest &req);
    void onApiTaskDetail(JZHttpServerClient *client, const JZHttpRequest &req);
    // A2A: 订阅一个 Task 的状态/产物事件(SSE, 终态自动关流)
    void onApiTaskSubscribe(JZHttpServerClient *client, const JZHttpRequest &req);
    // A2A: AgentCard(能力自述), 供管理台回答"这台 agent 能干什么"
    void onApiAgentCard(JZHttpServerClient *client, const JZHttpRequest &req);
    // 单个 agent 的详情(在线 / 离线但有记录 / 内置 agent / 本地子进程)
    void onApiAgentDetail(JZHttpServerClient *client, const JZHttpRequest &req);
    // 从"曾经连过"的列表里删掉一台(默认保留它的任务记录, withTasks=1 连记录一起删)
    void onApiAgentForget(JZHttpServerClient *client, const JZHttpRequest &req);
    // 页面: web/<name>, 找不到回 404; onDashboard = servePage("index.html")
    void servePage(JZHttpServerClient *client, const JZHttpRequest &req, const std::string &name);
    void onDashboard(JZHttpServerClient *client, const JZHttpRequest &req);

private:
    void startHttp();
    void startHub();                     // 起控制面(JZNetServer)
    void onHubPacket(uint64_t conn, JZNetPackPtr pack);
    void onHubDisconnected(uint64_t conn);
    void dropAgent(uint64_t conn, const std::string &reason);   // 断开/心跳超时共用
    void reapDeadAgents();                                      // 心跳回收(20s)
    JZTimer *m_reapTimer = nullptr;
    void failPendingTasksOf(const std::string &agentId, const std::string &error);
    Json::Value onlineAgentsJson() const;
    std::string resolveProgram(const JZCliServerAgentConfig &cfg) const;
    void appendLog(JZCliServerAgentInstance *inst, const std::string &chunk);
    // 管理台网页的落盘目录(<exeDir>/server_web); 页面 CSS/JS 也从这里读
    std::string dashboardDir() const;
    bool loadDashboardHtml(std::string &html, std::string &error) const;

    JZCliServerConfig m_config;
    JZHttpServer *m_http = nullptr;

    // init() 时读入的 PEM 内容(证书文件读不到就直接启动失败, 不静默降级成明文 HTTP)
    std::string m_httpCertPem;
    std::string m_httpKeyPem;
    std::string m_httpCaPem;
    std::map<std::string, JZCliServerAgentInstance*> m_agents;  // name -> 实例
    int64_t m_startTime = 0;

    // ---- 控制面 ----
    struct PendingTask
    {
        std::string taskId;
        std::string agentId;
        JZHttpServerClient *client = nullptr;   // 提交方的连接(断开后不再回包)
        JZTimer *timer = nullptr;               // 任务超时(到点回 504 并请求 cli 取消)
        int64_t startMs = 0;
    };

    JZNetServer *m_hub = nullptr;
    std::map<uint64_t, JZCliServerOnlineAgent> m_onlineByConn;
    std::map<std::string, uint64_t> m_connByAgent;        // agentId -> conn
    std::map<std::string, PendingTask> m_pendingTasks;    // taskId -> 在跑任务(client 可空, 均有超时)
    int m_taskSeq = 0;

    // ---- 内置 agent(server 自己跑的那个 JZAgent) ----
    JZServerAgent *m_selfAgent = nullptr;
    bool isSelfAgentId(const std::string &id) const;
    void onSelfAgentEvent(const JZServerAgentEvent &ev);
    // 任务下发核心: HTTP 与内置 agent 的工具共用。
    //   waiter 非空 = 同步模式(调用方不要自己回包, 回包在 taskEnd/done 时发出)
    //   返回 http 状态码; 202 = 已接受(异步), >=400 = 失败(ack 里有 error)
    int submitTask(const Json::Value &body, JZHttpServerClient *waiter,
                   Json::Value &ack, std::string &error);

    // ---- 任务记录 ----
    // 归属(见 docs/agent-hub-design.md §10): **server 是任务历史的主人**, cli 只知道
    // "当前这一条"加上它自己内存里的 recent(重启即丢)。历史不在 server 落盘的话,
    // cli 一重启, 提交方就再也查不到自己交过什么。
    // 只有一套模型: A2A v1.0 Task。内部字段名也用状态词本身, 不再存 running/success 那套。
    struct TaskRecord
    {
        std::string taskId, agentId, prompt, verifyCmd;
        std::string state;         // TASK_STATE_SUBMITTED / WORKING / COMPLETED / FAILED / CANCELED / REJECTED / UNSPECIFIED
        std::string error;         // TaskStatus.message
        std::string contextId;
        std::string lastEventType;
        int rounds = 0, round = 0;
        long long startMs = 0, endMs = 0, statusMs = 0;
        int eventSeq = 0;                          // 事件序号(断线重连可据此去重)
        std::vector<std::string> events;           // 有界状态事件(JSON 字符串, 放 metadata.events)
        std::vector<std::string> artifacts;        // A2A Artifact(JSON 字符串)
        std::vector<std::string> history;          // A2A Message(JSON 字符串)
    };
    std::map<std::string, TaskRecord> m_tasks;      // taskId -> 记录
    std::vector<std::string> m_taskOrder;           // 插入顺序(裁剪最老的)
    std::string m_taskFile;                         // <exeDir>/data/tasks.jsonl

    // GET /api/tasks/subscribe?id= 的订阅者: taskId -> 若干 SSE 连接
    std::map<std::string, std::vector<JZHttpServerClient*> > m_taskStreams;
    JZTimer *m_streamTimer = nullptr;
    void removeTaskStream(JZHttpServerClient *client);
    void broadcastTaskEvent(const std::string &taskId, const std::string &event,
                            const std::string &payload);
    Json::Value agentCardJson(const std::string &agentId) const;

    void loadTaskStore();
    void saveTaskRecord(const TaskRecord &r);       // 追加一行(JSONL, 后写覆盖先写)
    void rewriteTaskStore();                        // 删除记录后整体重写(否则重启会被旧行复活)
    void recordCreate(const std::string &taskId, const std::string &agentId, const Json::Value &body);
    void recordEnd(const std::string &taskId, const Json::Value &result);
    void recordFail(const std::string &taskId, const std::string &state, const std::string &error);
    void recordEvent(const std::string &taskId, const std::string &type, const std::string &data);
    void appendTaskEvent(TaskRecord *r, const std::string &type, const Json::Value &data);
    void finishTaskStreams(const std::string &taskId);
    TaskRecord *findTask(const std::string &taskId);
    Json::Value taskJson(const TaskRecord &r) const;
    Json::Value tasksJson(const std::string &agentId, const std::string &state, int limit,
                          const std::string &contextId = std::string()) const;

    // ---- 曾经连过的 agent(离线也留着, 列表要能回答"这台以前是什么") ----
    // 只有 server 记得住: cli 离线后没有任何地方能报出自己的信息, 而管理台恰恰在
    // "它已经掉线了"的时候最需要知道它原本是哪台、最后一次什么时候在。
    struct KnownAgent
    {
        std::string agentId, name, version, rootDir, modelName;
        long long firstSeen = 0, lastSeen = 0;
        int connects = 0;
    };
    std::map<std::string, KnownAgent> m_known;
    std::string m_agentFile;                 // <exeDir>/data/agents.json
    void loadAgentStore();
    void saveAgentStore();
    void upsertKnown(const KnownAgent &info);
    Json::Value knownAgentsJson() const;     // 含离线 + 内置 agent
};

#endif // !JZ_AGENT_CLI_SERVER_H_
