#ifndef JZ_AGENT_CLI_H_
#define JZ_AGENT_CLI_H_

#include <atomic>
#include <deque>
#include <functional>
#include <string>
#include <map>
#include <vector>

#include "JZAgentCliTask.h"

class JZHttpServer;
class JZHttpServerClient;
class JZHttpRequest;
class JZHubLink;
class JZNodeLink;
class JZMcpClient;
class JZTimer;
class JZProcess;
class JZNetworkAccessManager;

// ---------------------------------------------------------------------------
// MCP 客户端配置: 连一个外部 MCP 服务器, 把它的远端工具挂进本 agent 的工具表。
//
//   "动态增加" = **只改配置(不重编译/不改代码)** —— 在 config.json 的 mcp.servers
//   里加减一条, 重启进程即可; 启动时逐个异步连接, 连上才登记工具, 连不上只记日志。
//
//   ⚠ 三条必须知道的约束:
//     ① 连接时机: JZAgent 只允许在 Inactive(**首个任务之前**)改工具表与系统提示词
//        (改工具表会破坏会话的 KV 前缀缓存, 见 JZAgent::rejectWhileActive), 所以
//        MCP 是"启动时挂一次"的: 任务已经开跑之后再连上也挂不进去(只记警告);
//     ② 执行位置: 远端工具在**对端**执行, 不经过本机 terminal 的区域/路径审核
//        (那套只约束本地工具), 等于把对端的能力引进来 —— 所以有总开关 mcp.enabled;
//     ③ 命名: 本地工具名默认 "<serverName>__<工具名>"(serverName 由对端 serverInfo
//        给), 与本地工具重名的会被跳过(JZAgent::addMcp 只打 warning)。
// ---------------------------------------------------------------------------
class JZAgentCli : public JZObject
{
    friend class JZAgentCliTask;   // 任务只借宿主的出口发事件/查配置, 自己不碰网络
public:
    explicit JZAgentCli(JZObject *parent = nullptr);
    ~JZAgentCli() override;

    // 建 JZAgent、注册工具、设 system prompt。必须在事件循环起来前调用。
    bool init(const JZCliConfig &config, std::string &error);

    // 起 HTTP 服务: 暴露 POST /task 提交任务 + 验证脚本门控循环。
    void startHttp();

    // 按配置(nodes[])连接各 link 模式 agent_node(异步; 断线自动重连)。
    // 必须在事件循环起来前调用; 没配 node 时什么都不做(纯本地执行)。
    void startNodes();

    // 起控制面(managed 模式): JZNetClient 连 server, 上报 hello/心跳, 收 taskSubmit/taskCancel/reset。
    // 必须在事件循环起来前调用; 未配置 server.host 时什么都不做(standalone)。
    // ⚠ managed 下**不**调 startHttp(): 任务只从控制面来, 对外不开 HTTP。
    void startServerLink();
    bool hubOnline() const;
    JZHubLink *hub() const { return m_hub; }

    // remote_ssh 的 host(节点名) -> 连接(名字为空或找不到返回 nullptr); 供工具与自检使用
    JZNodeLink *nodeLink(const std::string &name) const;
    // 节点状态(JSON 数组): /info、控制面 hello/心跳、管理台共用同一份口径
    Json::Value nodesJson() const;

    // 起 MCP 客户端: 按配置(mcp.servers)逐个**异步**连接外部 MCP 服务器, 连上后把
    // 远端工具登记进 agent(见 JZMcpClient / JZAgent::addMcp)。
    // 要在事件循环起来前调用(越早发起越好), 连接结果异步到达: 失败只记日志, 不影响启动;
    // 连接晚于首个任务则挂不上(JZAgent 只在 Inactive 允许改工具表), 只记一条警告。
    void startMcp();

    // /task 请求入口(由内部 TaskHandler 调用)
    void onTaskSubmit(JZHttpServerClient *client, const JZHttpRequest &req);

    // 网页控制台: 列出工作区内可当验证脚本的文件(GET /scripts)
    void onListScripts(JZHttpServerClient *client, const JZHttpRequest &req);

    // 网页控制台: 运行信息(GET /info) —— 顶栏显示模型地址等; SSE hello 帧带同一份数据
    //   {agent, version, modelUrl, modelName, rootDir, busy, round, mcp[]}。刻意不含 modelToken。
    //   mcp[] = 每个配置的 MCP 服务器当前状态(挂上了没有/几个工具/什么错), 没网页面板时
    //   可以用它自检(curl /info)。
    Json::Value makeInfo() const;
    void onInfo(JZHttpServerClient *client, const JZHttpRequest &req);

    // 网页控制台: 重置(POST /reset)。body: {"scope":"task"|"history"}
    //   task    = 中止当前任务(在途请求回 status:"cancelled") + 重置会话
    //   history = 只重置会话(清对话历史), 不动在途任务
    // 两者最终都调 JZAgent::reset(): 清 LLM 上下文/用量/待发请求, 下次提交从干净起点开始。
    void onReset(JZHttpServerClient *client, const JZHttpRequest &req);

    // 网页控制台: 实时事件流(GET /events, Server-Sent Events)
    //   把 agent 的 sigError / 验证结果 / 任务起止实时推给网页 —— 否则任务跑很久时
    //   界面只能干等到 /task 的一次性回包, 中途报错无从得知。
    //   帧格式: data: {"type":"...","data":{...}}
    //   type: hello / state / task_start / thinking / reply / tool_call / tool_result /
    //         error / verify / task_end / reset
    //   state: 新连接时若正有任务在跑, 推一份当前状态快照(让刚打开网页的人看到进行中的任务);
    //          data = {busy, round, verify_cmd, prompt, reply, think, last_verify_output, tools[]}
    void onEvents(JZHttpServerClient *client, const JZHttpRequest &req);
    void pushEvent(const std::string &type, const Json::Value &data);   // 推给所有订阅者
    void removeEventClient(JZHttpServerClient *client);

    // ------------------------------------------------------------------
    // A2A 网页接口 —— 与 server 管理台**同一套 /api/***, 前端共用一份页面。
    //   单 agent 的"当前任务 + 最近历史"映射成 A2A Task; 内部仍然走 submitTask
    //   的队列/验证循环, 这里只做形状转换, 不另做一套执行逻辑。
    //     GET  /api/tasks              列表(当前任务在最前)
    //     POST /api/tasks              SendMessage(立即回 202 + Task)
    //     GET  /api/tasks/detail?id=
    //     POST /api/tasks/cancel?id=
    //     POST /api/tasks/delete       删一条已结束的历史记录(body {"id":"h<seq>"})
    //     GET  /api/tasks/subscribe?id= SSE(task / status_update / artifact_update)
    //     GET  /api/agent              本机 agent 的信息 + AgentCard
    //     GET  /api/status             只有自己一台(首页列表页复用)
    // ------------------------------------------------------------------
    void onApiTasks(JZHttpServerClient *client, const JZHttpRequest &req);
    void onApiTaskDetail(JZHttpServerClient *client, const JZHttpRequest &req);
    void onApiTaskCancel(JZHttpServerClient *client, const JZHttpRequest &req);
    void onApiTaskDelete(JZHttpServerClient *client, const JZHttpRequest &req);
    void onApiTaskSubscribe(JZHttpServerClient *client, const JZHttpRequest &req);
    void onApiAgent(JZHttpServerClient *client, const JZHttpRequest &req);
    void onApiStatus(JZHttpServerClient *client, const JZHttpRequest &req);
    // 网页: <httpDir>/agent.html(单 agent 的对话页; 与 server 的 /agent 是同一份)
    void onDashboard(JZHttpServerClient *client, const JZHttpRequest &req);

    // 网页"后台任务"面板(给人看的, 不占模型的上下文):
    //   GET  /terminal              -> {running, session, cwd}
    //   POST /terminal/cancel              -> {ok,killed}  (单前台任务: 停当前那条)
    void onTerminalList(JZHttpServerClient *client, const JZHttpRequest &req);
    void onTerminalCancel(JZHttpServerClient *client, const JZHttpRequest &req);

    // 任务提交核心: 由 HTTP(onTaskSubmit) 调用, 解析 prompt / 验证脚本 / 轮次, 进入"提交→验证"循环。
    // respond 在循环结束(成功/失败/取消)时被调用一次, 带回 JSON 结果
    //   {status, verify_rounds, agent_reply, last_verify_output, error?}。
    void submitTask(const Json::Value &req, std::function<void(const Json::Value &)> respond);

    // ---- 任务池(动态创建 + 空闲回收) ----
    //   HTTP / hub 提交时才创建 task; 任务结束标记空闲; 超过 keepAlive 且空闲超过
    //   idleRecycleSec 的 task 由定时扫描回收。queues 是同时活动任务数的上限。
    // 新建一个任务对象(不复用); wantRoot 是这条任务的工作目录(空 = m_config.rootDir)。
    // 到 task.queues 上限时先丢掉已结束的对象; 还满就返回 nullptr 让调用方排队。
    JZAgentCliTask *acquireTask(const std::string &wantRoot, std::string &error);
    // 任务结束: 从池里摘掉并 deleteLater(一次性对象)
    void releaseTask(JZAgentCliTask *task);
    // 任务请求里的 rootDir(网页上切换目录): 空 = 用配置; 不是目录则返回空并给 error
    std::string resolveRootDir(const Json::Value &req, std::string &error) const;
    void recycleIdleTasks();

    // ---- 任务队列(task.queues) ----
    // 每个队列一个独立 JZAgent(独立会话/上下文), 队列内串行、队列间并发。
    Json::Value queuesJson() const;          // /info 用: [{queue, busy, round, taskId}]
    bool anyBusy() const;                    // 是否还有队列在跑(管理台/测试用)
    int pendingCount() const;                // 排队中的任务数
    int queueCount() const { return (int)m_tasks.size(); }

    // 生效的程序映射(即配置里传进来的那份), 供宿主打印/自检
    const std::map<std::string, std::string> &programPaths() const { return m_config.programPaths; }

protected:
    void onTaskReply(JZAgentCliTask* task, const std::string& text);
    void onTaskThinking(JZAgentCliTask* task, const std::string& text);
    void onTaskToolCall(JZAgentCliTask* task, const ToolCall& call);
    void onTaskToolResult(JZAgentCliTask* task, const ToolCallResult& result);
    void onTaskError(JZAgentCliTask* task,const std::string& err);
    void onTaskFinished(JZAgentCliTask *task);

    void setupAgent();
    std::string buildSystemPrompt() const;

    // ---- 任务队列 ----
    bool initWorkers(std::string &error);     // queues>1: 建"工作实例"(每个一套 JZAgent + 工具)
    void runTask(const Json::Value &req, std::function<void(const Json::Value &)> respond);  // 引擎本体
    void dispatchTo(JZAgentCliTask *task, const Json::Value &req, const std::string &taskId,
                    std::function<void(const Json::Value &)> respond);
    void pumpQueue();                         // 有队列空出来时派发排队任务
    int idleWorker() const;                   // 找一个空闲队列, 没有返回 -1
    void onWorkerEvent(int queue, const std::string &type, const Json::Value &data);
    void emitLocalEvent(const std::string &type, const Json::Value &data);   // 只推给 SSE 订阅者
    void syncHubState();                            // 汇总所有队列的忙闲/当前任务并立即上报
    void cancelNodeExec(const void *owner);          // 只取消指定队列发起的远端命令

    // 一次 MCP 连接结束(成功/失败都到这儿): 就绪则把它挂进 agent(登记远端工具),
    // 失败/挂不上则记日志并销毁这个 client。index = 它在 m_config.mcpServers 里的位置
    // (用来回填 m_mcpStates)。末尾会调 finishMcpPending() 记账。
    void onMcpFinished(JZMcpClient *client, int index);
    // 一个 MCP 连接流程结束了: 未结束数 -1, 减到 0 表示"MCP 初始化完毕"(放行新任务)。
    void finishMcpPending();

    // ---- 长期记忆(跨任务) ----
    // 记忆文件全路径: 配置 memory.file 优先(相对路径按 rootDir 解析), 空则 <rootDir>/.agent_memory.md
    std::string memoryFilePath() const;
    // 开工时注入任务消息的记忆头部(未启用/还没有记忆时返回空串)。注入与 memory read 同一份逻辑。
    std::string buildMemoryBlock() const;

    std::string extractUserText(const Json::Value &params);
    // 收集 /task 的图片附件(多模态): images 数组 或 messages 里的 image_url 部分。
    // 返回可直喂模型的 URL 列表; 有格式/大小/数量问题时写 error(调用方据此回 400)。
    std::vector<std::string> extractImages(const Json::Value &params, std::string &error);

    // q0 看本地 SSE 或在线 hub; worker 看 master 的 eventSink(最终消费者由 master 统一分发)。
    bool hasEventConsumer() const
    {
        return !m_eventClients.empty() || hubOnline() || (bool)m_eventSink;
    }

    // HTTP 传输 + 验证脚本门控循环
    std::string deriveVerifyCmd(const std::string &script, const std::string &explicitCmd) const;

    void respondTask(const std::string &status, int rounds, const std::string &reply,
                    const std::string &verifyOut, const std::string &err);
    bool checkToken(const JZHttpRequest &req) const;      // ?token= / Authorization: Bearer
    void resetSession(bool cancelRunning);                // 重置会话(可选先中止在途任务)
    // master: 取消全部排队与在跑任务; worker: 只取消自己。单任务取消走 cancelLocalTask,
    // 避免 q0 的 taskCancel 误停其他队列。
    bool cancelTask(const std::string &reason);
    bool cancelLocalTask(const std::string &reason);
    void resetHistory();

    // 控制面下发(由 JZHubLink 回调)
    void onHubTaskSubmit(const std::string &taskId, const Json::Value &body);
    void onHubTaskCancel(const std::string &taskId);
    void onHubReset(const std::string &scope);
    std::string nodesJsonString() const;
    void endLoop();
    void onClientDestroyed(JZObject *);
    void sendJson(JZHttpServerClient *client, int code, const Json::Value &j);

    JZCliConfig m_config;
    
    std::vector<JZNodeLink *> m_nodeLinks;    // 按 nodes[] 顺序(启动时建; 本对象所有)
    std::vector<JZCliMcpState> m_mcpStates;   // 每个 MCP server 的连接/挂载状态(见 startMcp)
    std::vector<JZMcpClient *> m_mcpClients;  // 已连上的 MCP: 任务对象动态创建时挂进它的 agent
    // MCP 初始化闸门: 配置里的 server **全部有结果**(连上 / 连不上)之前拒绝新任务 ——
    // 否则任务先跑起来, 稍后连上的远端工具就挂不进 agent 了(只能重启进程, 见 onMcpFinished)。
    bool m_mcpInitDone = true;   // 没配 MCP(或没启用)时恒为 true
    int m_mcpPending = 0;        // 还在连接中的 server 数(发起就失败的算已结束, 不计数)
    JZVirtualTerminal *m_terminal = nullptr;
    // 联网工具(web_search / web_fetch)共享的访问管理器 —— 代理只在它上面设一次。
    // 是本对象的子对象; 没挂网络工具时为空。
    JZNetworkAccessManager *m_netMan = nullptr;

    // HTTP 传输
    JZHttpServer *m_http = nullptr;
    JZTimer *m_recycleTimer = nullptr;   // 空闲任务对象周期回收

    // 控制面(managed 模式): 到 server 的连接。standalone 下为空。
    JZHubLink *m_hub = nullptr;

    // ---- 任务队列(task.queues) ----
    std::vector<JZAgentCliTask *> m_tasks;       // [0] = this; 其余由 initWorkers() 建
    struct QueuedTask
    {
        Json::Value req;
        std::string taskId;
        std::string rootDir;      // 这条任务的工作目录(入队时已解析校验)
        std::function<void(const Json::Value &)> respond;
    };
    // 任务对象是**一次性**的: 派发时新建, 回完包就销毁。m_tasks 里只有"正在跑"的。
    std::deque<QueuedTask> m_pending;          // 全忙时排队(FIFO 派发)
    std::string m_queueName = "q0";
    // worker -> master 的事件出口(带队列下标); master 自己为 null
    std::function<void(int, const std::string &, const Json::Value &)> m_eventSink;
    // 节点路由: worker 自己不连 node, 用 master 的那批连接(见 nodeLink())
    std::function<void(const void *)> m_nodeCancel;
    bool m_cancelling = false;                      // master 持有: 全局/单任务取消收尾期间禁止重入派发
    bool m_pumping = false;                         // master 持有: pumpQueue 重入闸(同步拒绝回调会再次调用)
    // 控制面任务 id(server 生成)。非空 = 当前任务是 server 派下来的,
    // 事件流与回包都按它带回 server; HTTP 任务为空。
    std::string m_currentTaskId;

    JZHttpServerClient *m_pendingClient = nullptr;   // 当前在跑任务对应的 HTTP 连接(循环结束回包)
   

    // 最近一次任务的回包。提交方(POST /task 的那条连接)断连后任务仍在跑完 —— 那时结果只能
    // 经 SSE 出口; 若订阅者也恰好不在(网页关了又开), 重连时的 hello 帧带上它, 让界面收尾。
    Json::Value m_lastResult;
    bool m_hasLastResult = false;

    // 当前状态快照(SSE 新连接时推过去, 让刚打开网页的人看到正在跑的任务)
    std::string m_currentPrompt;                     // 当前任务 prompt
    std::string m_accumThink;                        // 累积本轮思考(thinking)
    bool m_thinkLogged = false;                    // 本轮 think 是否已落 Agent 日志(每单元一条)
    std::vector<JZCliToolSnap> m_toolSnaps;          // 本轮已发生的工具调用/结果
    std::string m_lastFeedbackSig;                   // 上一轮"喂回模型"的失败签名(退出码+输出)
    int m_sameFailCount = 0;                         // 连续拿到完全一致失败的次数(识别原地打转)
    bool m_sessionStarted = false;                   // 是否已发起过首轮 request(避免首轮 reset 告警)

    // 最近任务历史: 长程任务跑几十分钟很常见, 网页不会一直开着 —— 服务端记住最近若干次
    // 请求与它们的结果, 新连接(hello 帧)或 GET /info 时以 recent 数组吐出去, 界面据此重建。
    std::vector<JZCliHistoryItem> m_history;   // 最旧在前, 上限 kHistoryMax
    int m_historySeq = 0;
    static const int kHistoryMax = 20;
    void pushHistory(const std::string &prompt, const std::string &verifyCmd);
    void finishHistory(const std::string &status, int rounds, const std::string &reply,
                       const std::string &verifyOut, const std::string &err);
    Json::Value recentJson() const;            // 最近在前(与界面"新的在上"一致)
    static int64_t nowMs();

    // 传输无关扩展
    std::function<void(const Json::Value &)> m_respond;  // 循环结束的回包回调

    // SSE 订阅者(GET /events): 由连接析构回调摘除
    std::vector<JZHttpServerClient *> m_eventClients;

    // ---- A2A 形状(给网页看的一层, 不改变内部任务模型) ----
    struct A2aTask
    {
        std::string id, prompt, verifyCmd, state, reply, verifyOut, error;
        long long startMs = 0, endMs = 0;
        int rounds = 0, eventSeq = 0;
        std::vector<Json::Value> events;      // 有界状态事件(只在本次运行期间留存)
    };
    A2aTask m_a2aLive;                        // 当前这条任务(active 时有效)
    bool m_a2aLiveActive = false;
    int m_a2aSeq = 0;
    std::map<JZHttpServerClient *, std::string> m_a2aSubs;   // SSE 订阅: 连接 -> taskId
    std::string a2aAgentId() const;
    Json::Value a2aTaskJson(const A2aTask &t) const;
    A2aTask a2aTaskFromRecent(const Json::Value &it) const;  // recent 一条 -> A2A
    Json::Value a2aHistoryJson(int limit) const;             // 最近历史 -> A2A Task 数组
    Json::Value a2aFindTask(const std::string &id, bool &live) const;
    void a2aEmit(const std::string &type, const Json::Value &data);   // 给订阅者推一帧
    void a2aFinish(const Json::Value &result);                        // 收尾: 落终态 + 关流
};

#endif // !JZ_AGENT_CLI_H_
