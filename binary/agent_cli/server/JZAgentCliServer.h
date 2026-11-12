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
    std::string project;       // 项目名(cli 上报): 一个 cli 一个 project
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
    // 日志文件路径(可执行文件相对或绝对); 空 = <exeDir>/data/logs, 文件名固定 Server.log
    std::string logFile;

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
    // 删除历史任务记录(只允许已结束的; 在跑的必须先取消)
    void onApiTaskDelete(JZHttpServerClient *client, const JZHttpRequest &req);
    // 任务记录(server 侧历史, 落盘 tasks.jsonl)
    void onApiTaskList(JZHttpServerClient *client, const JZHttpRequest &req);
    void onApiTaskDetail(JZHttpServerClient *client, const JZHttpRequest &req);
    // 往前翻一页事件("加载更多"): ?id=&before=<offset>&limit=
    void onApiTaskEvents(JZHttpServerClient *client, const JZHttpRequest &req);
    // A2A: 订阅一个 Task 的状态/产物事件(SSE, 终态自动关流)
    // ⚠ 网页已改用下面的常连, 这个接口只为兼容外部调用方保留
    void onApiTaskSubscribe(JZHttpServerClient *client, const JZHttpRequest &req);
    // 网页常连(下行): /api/events 一条推全部(任务事件 + agent 快照 + 删除通知)
    void onApiEvents(JZHttpServerClient *client, const JZHttpRequest &req);
    // A2A: AgentCard(能力自述), 供管理台回答"这台 agent 能干什么"
    void onApiAgentCard(JZHttpServerClient *client, const JZHttpRequest &req);
    // 单个 agent 的详情(在线 / 离线但有记录 / 内置 agent / 本地子进程)
    void onApiAgentDetail(JZHttpServerClient *client, const JZHttpRequest &req);
    // 目录浏览(网页上切换工作目录): 列 server 本机目录
    void onApiFs(JZHttpServerClient *client, const JZHttpRequest &req);
    // 文件内容(原始字节, 不解码): 前端自己 TextDecoder 判 UTF-8 / GB18030
    void onApiFsRaw(JZHttpServerClient *client, const JZHttpRequest &req);
    // 主机视图: 每个 agent + 它用过的目录(从任务记录聚合, 很轻)
    void onApiProjects(JZHttpServerClient *client, const JZHttpRequest &req);
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
    // 任务"无响应"巡检(同一个 5s 定时器驱动): 距最后一条事件超过 timeoutMs 就判失败并取消。
    void reapStuckTasks();
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
        // 无响应判定: **距最后一条事件**超过 timeoutMs 才算超时(每来一条事件就刷新时间),
        // 不是从提交起的墙钟 —— 慢但一直在干事(推 reply / tool_result)的任务不该被杀。
        // 时间用 JZPlatform::getTickCountMs() 算, 由 5s 巡检(reapStuckTasks)判, 不给每个任务
        // 单独开 JZTimer(流式事件每秒几十条, stop/start 太浪费, 还撞"已启动不能重入")。
        int64_t startMs = 0;       // 墙上钟(展示用)
        int64_t lastEventMs = 0;   // tick 计数(比较用)
        int timeoutMs = 0;
    };

    // ---- 网页长连接(下行): 每个页面**一条**, 从打开到关闭都不主动断 ----
    //   以前是"每条任务开一条 SSE, 终态由服务端关流" —— 每跑完一个任务浏览器就要
    //   重连一次, 看着就是"连接中断"。现在一条常连推所有东西: 任务事件(帧里带 taskId)、
    //   agent 快照(event: agents)、删除通知(event: deleted)。事件名已经能区分类型,
    //   拆成两条连接只是多占一个同域连接 + 多一份保活/回收逻辑, 没有收益。
    //   上行(提交/取消/删除)仍是普通 POST —— SSE 单向。
    std::vector<JZHttpServerClient*> m_pageStreams;    // GET /api/events
    void openPageStream(JZHttpServerClient *client);
    void removePageStream(JZHttpServerClient *client);
    void broadcastPageEvent(const std::string &name, const Json::Value &v);
    void broadcastAgentEvent();

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
    //   origin: "user" = 人主动下发(HTTP/管理台), "agent" = 内置 agent 的工具内部派活。
    //          管理台的任务日志只列 user, 内部调用只作为父任务的一步出现。
    int submitTask(const Json::Value &body, JZHttpServerClient *waiter,
                   Json::Value &ack, std::string &error,
                   const std::string &origin = std::string("user"),
                   const std::string &parentTaskId = std::string());

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
        std::string project;       // 项目名(来自 agent): 网页按 project 筛选任务
        std::string origin;        // "user"(人下发) / "agent"(内置 agent 工具派活)
        std::string parentTaskId;  // origin=agent 时: 是哪个任务调的工具
        std::string lastEventType;
        int rounds = 0, round = 0;
        long long startMs = 0, endMs = 0, statusMs = 0;
        int eventSeq = 0;                          // 事件序号(断线重连可据此去重)
        std::vector<std::string> events;           // 状态事件(JSON 字符串, 放 metadata.events), 全量不裁剪
        // ---- 流式分片合并(落盘前) ----
        // reply/thinking 是逐 token 推送的, 一条一条写会把 events 撑成几万个碎片。
        // 这里把"连续的同类型分片"攒成一整段, 遇到别的类型/任务收尾才落一条。
        std::string openKind;                      // 正在累积的类型(reply/thinking); 空 = 没有
        std::string openText;                      // 已累积的文本
        long long openAt = 0;                      // 首片时间(落盘时当 at 用)
        std::vector<std::string> artifacts;        // A2A Artifact(JSON 字符串)
        std::vector<std::string> history;          // A2A Message(JSON 字符串)
        // 注意: 对话上下文**不在这里**。每条任务存一份快照会让 JSONL 涨成 O(n²),
        // 而续聊只要"这条会话最新一份"就够(见下面的 ctx 文件)。
    };
    std::map<std::string, TaskRecord> m_tasks;      // taskId -> 记录
    std::map<std::string, long long> m_sseTick;     // taskId -> 上次推 status_update 的 tick(节流)
    std::vector<std::string> m_taskOrder;           // 插入顺序(裁剪最老的)
    std::string m_taskFile;                         // <exeDir>/data/tasks.jsonl

    // GET /api/tasks/subscribe?id= 的订阅者: taskId -> 若干 SSE 连接
    std::map<std::string, std::vector<JZHttpServerClient*> > m_taskStreams;
    JZTimer *m_streamTimer = nullptr;
    void removeTaskStream(JZHttpServerClient *client);
    void dropTaskStreams(const std::string &taskId);   // 记录已删: 推 deleted 后关流
    void broadcastTaskEvent(const std::string &taskId, const std::string &event,
                            const std::string &payload);
    Json::Value agentCardJson(const std::string &agentId) const;

    void loadTaskStore();
    void saveTaskRecord(const TaskRecord &r);       // 追加一行(JSONL, 后写覆盖先写)
    void rewriteTaskStore();                        // 删除记录后整体重写(否则重启会被旧行复活)
    void recordCreate(const std::string &taskId, const std::string &agentId, const Json::Value &body,
                      const std::string &origin = std::string("user"),
                      const std::string &parentTaskId = std::string());
    // 「接着说」: 请求带 taskId 时复用同一条记录, 追加一轮(user 消息 + user_msg 事件)
    void recordContinue(const std::string &taskId, const Json::Value &body);
    void recordEnd(const std::string &taskId, const Json::Value &result);
    void recordFail(const std::string &taskId, const std::string &state, const std::string &error);
    void recordEvent(const std::string &taskId, const std::string &type, const std::string &data);
    void appendTaskEvent(TaskRecord *r, const std::string &type, const Json::Value &data);
    // 把正在累积的 reply/thinking 段落写成一条事件(类型切换/超长/收尾时调用)
    void flushOpenSegment(TaskRecord *r);
    void finishTaskStreams(const std::string &taskId);
    TaskRecord *findTask(const std::string &taskId);
    // evLimit > 0: 只给最后一页(起点对齐 user_msg); <= 0: 全量(落盘/内部用)
    Json::Value taskJson(const TaskRecord &r, int evLimit = 200) const;
    // ---- 会话上下文存档: 一个 contextId 一个文件, 只留最新一份 ----
    // 为什么不塞进任务记录: 同一会话链第 N 条再把前 N 轮存一遍 → tasks.jsonl 涨成 O(n²);
    // 而且续聊只需要"这条会话当前的状态", 不需要任意回溯某条历史任务。
    std::string m_ctxDir;                    // <exeDir>/data/ctx
    std::string contextFile(const std::string &contextId) const;
    void saveContextFile(const std::string &contextId, const Json::Value &ctx);
    Json::Value loadContextFile(const std::string &contextId) const;   // 没有 → null
    void dropContextIfUnused(const std::string &contextId);            // 这条会话的任务全删了才清文件
    // origin 留空 = 不过滤; "user" = 只要人下发的
    Json::Value tasksJson(const std::string &agentId, const std::string &state, int limit,
                          const std::string &contextId = std::string(),
                          const std::string &origin = std::string()) const;

    // ---- 曾经连过的 agent(离线也留着, 列表要能回答"这台以前是什么") ----
    // 只有 server 记得住: cli 离线后没有任何地方能报出自己的信息, 而管理台恰恰在
    // "它已经掉线了"的时候最需要知道它原本是哪台、最后一次什么时候在。
    struct KnownAgent
    {
        std::string agentId, name, version, rootDir, project, modelName;
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
