#ifndef JZ_AGENT_CLI_TASK_H_
#define JZ_AGENT_CLI_TASK_H_

#include <atomic>
#include <deque>
#include <functional>
#include <string>
#include <map>
#include <vector>

#include "JZObject.h"
#include "JZAgent.h"          // JZAgent / JZAgentConfig / ToolCall / ToolCallResult
#include "JZCodeTools.h"      // ICodeToolHost / JZVirtualTerminalConfig / TrustedDir
#include "JZVirtualTerminal.h"

class JZAgentTool;
class JZVirtualTerminal;
class JZNetworkAccessManager;
class JZHttpServerClient;
class JZProcess;
class JZNodeLink;
class JZAgentCli;

// ---------------------------------------------------------------------------
// 公共配置: 宿主(JZAgentCli)读一份, 每个任务(JZAgentCliTask)在 init 时拷一份。
//   放在这里是为了让任务层自包含, 不需要反向 include JZAgentCli.h。
// ---------------------------------------------------------------------------
struct JZCliMcpServer
{
    std::string name;
    std::string transport = "stdio";
    std::string command;
    std::vector<std::string> args;
    std::string url;
    std::string toolPrefix;
    int timeoutMs = 10000;
    int callTimeoutMs = 120000;
};

struct JZCliNodeConfig
{
    std::string name;      // 节点名(= remote_ssh 的 host 参数, 必填)
    std::string host;
    int port = 7500;
    std::string token;
};

struct JZCliServerLinkConfig
{
    std::string host;
    int port = 8090;
    std::string token;
    std::string agentId;
};

// 任务池: queues = 同时最多活动的任务数(上限); maxPending = 全忙时最多排队数;
//   keepAlive = 空闲任务保留几个(不回收); idleRecycleSec = 超出 keepAlive 的空闲任务
//   空闲超过这个秒数就回收(0 = 立即回收)。动态创建在 HTTP / hub 提交时进行。
struct JZCliTaskConfig
{
    int queues = 1;
    int maxPending = 32;
    int keepAlive = 1;
    int idleRecycleSec = 60;
};

struct JZCliConfig
{
    std::string agentName = "agent_cli";
    std::string version = "1.0.0";

    std::string modelUrl;
    std::string modelName;
    std::string modelToken;
    int netTimeout = 120 * 1000;
    int maxRounds = 100;
    int maxToken = 160000;
    // 思考开关(llm.thinking): Auto = 不发字段(默认, 最兼容) / On / Off = 显式发 enable_thinking
    ThinkMode thinkMode = ThinkMode::Auto;
    Json::Value extraBody;

    int maxRetries = 10;
    int maxFixRetries = 2;
    int retryDelayMs = 1000;

    bool loopDetect = true;
    int loopWindow = 24;
    int loopRepeat = 5;
    int loopSameResult = 5;
    int loopPollRepeat = 16;
    int loopPollSameResult = 6;
    int loopLineRepeat = 6;
    int loopBlockRepeat = 3;
    int loopPeriodMax = 256;
    int loopPeriodRepeat = 8;

    int verifyMaxRounds = 30;
    int verifyTimeoutMs = 60000;

    bool memoryEnabled = true;
    std::string memoryFile;
    int memoryMaxBytes = 8192;

    std::string systemExtra;
    std::string systemExtraFile;

    std::string rootDir;
    // ---- project ----
    // **project 就是 rootDir**: 一个 cli 一个工作区, 没有额外的"项目名"概念
    // (配置里的 `project.dir` 只是 `terminal.rootDir` 的别名, 两个都空 = 当前目录)。
    // 全局 `.jzagent`(settings 里的那份) 里的 skills 总是加载, 排在项目 skills 之后。
    std::string skillsDir;      // 全局 .jzagent 目录; 空 = %USERPROFILE%/.jzagent
    std::vector<TrustedDir> trustedDirs;
    std::vector<JZCliNodeConfig> nodes;

    bool allowTerminal = true;
    bool allowFileTools = true;

    // ---- 权限档位(三选一, 见 JZVirtualTerminalConfig.h) ----
    //   strict   : 只跑登记过的命令 + 区外只读 + 禁联网 + 禁装依赖
    //   standard : 未登记命令放行 + 区外只读 + 允许联网 + 禁装依赖   ← 默认
    //   loose    : standard + 允许装/卸依赖 + 授信目录可写
    // 安全底线(提权/关机/格式化/删工作区外)三档都拦, 没有开关。
    PermissionProfile profile = PermissionProfile::Standard;
    // 下面三个是"在档位上单独覆盖"的高级用法: 配置里写了才算(has* = 是否显式配置)。
    bool allowPackageManager = false;
    bool allowNetwork = true;
    bool allowUnknownPrograms = true;
    bool hasAllowPackageManager = false;
    bool hasAllowNetwork = false;
    bool hasAllowUnknownPrograms = false;

    // 联网工具(web_search / web_fetch)。默认**跟随权限档位**: 严格档(禁联网)不注册,
    // 默认/宽松档注册; 配置里显式写了 network.enabled 才以配置为准(hasNetworkTools)。
    bool allowNetworkTools = true;
    bool hasNetworkTools = false;
    std::string webSearchUrl;
    std::string proxyHost;
    int proxyPort = 0;
    std::string proxyUser;
    std::string proxyPassword;

    bool mcpEnabled = true;
    std::vector<JZCliMcpServer> mcpServers;

    std::string shellProgram;
    int recycleSec = 15 * 60;
    std::string logFile;
    std::map<std::string, std::string> programPaths;
    std::map<std::string, std::string> env;

    std::string httpBind = "127.0.0.1";
    int httpPort = 8080;
    std::string httpToken;
    std::string httpDir;

    JZCliTaskConfig task;
    JZCliServerLinkConfig server;

    bool managed() const { return !server.host.empty(); }
};

// 一个 MCP 服务器在本进程里的状态(配置里配了几个就有几条)。
struct JZCliMcpState
{
    std::string name;
    std::string transport;
    std::string target;
    bool ready = false;
    int tools = 0;
    std::string error;
};

// 本轮已发生的工具调用(供 SSE 连接时做当前状态快照)
struct JZCliToolSnap
{
    std::string id;
    std::string name;
    std::string args;
    bool hasResult = false;
    bool ok = false;
    std::string result;
};

// 最近任务的一条记录(长程任务: 网页不会一直开着, 重连时靠 hello / GET /info 的 recent 补回界面)
struct JZCliHistoryItem
{
    int seq = 0;
    std::string prompt;
    std::string verifyCmd;
    std::string status;
    std::string reply;
    std::string verifyOut;
    std::string error;
    int rounds = 0;
    int64_t startMs = 0;
    int64_t endMs = 0;
};

// ---------------------------------------------------------------------------
// JZCliToolHost —— agent_cli 侧的工具宿主(每个任务一份)。
//   给 agent_tool 的通用工具喂区域配置 + 读文件(直读, 无缓存)。
// ---------------------------------------------------------------------------
class JZCliToolHost : public ICodeToolHost
{
public:
    explicit JZCliToolHost(const JZCliConfig *config);

    void refresh();

    const JZVirtualTerminalConfig &toolTerminalConfig() const override { return m_terminalConfig; }
    bool toolReadFile(const std::string &absPath, std::string &text, std::string &error) override;

private:
    const JZCliConfig *m_config = nullptr;
    JZVirtualTerminalConfig m_terminalConfig;
};

// ---------------------------------------------------------------------------
// JZAgentCliTask —— 一个任务的执行体(未来可放独立线程)。
//   一个 JZAgent + 一套工具/终端 + 一份任务状态; 一个 task 同一时刻只跑一个任务。
//   与宿主之间只通过 m_cli 回调/信号通信: 事件、结果、忙闲、node 路由都由宿主统一出口,
//   自身不碰 HTTP / hub / SSE / MCP 连接。
// ---------------------------------------------------------------------------
class JZAgentCliTask : public JZObject
{
public:
    explicit JZAgentCliTask(JZAgentCli *cli, JZObject *parent = nullptr);
    ~JZAgentCliTask() override;

    bool init(const JZCliConfig &config, std::string &error);

    // 引擎入口: 一次"提交 → 验证 → 重试"循环; 结束时 respond 回调一次。
    void runTask(const Json::Value &req, std::function<void(const Json::Value &)> respond);

    // ---- 任务池状态(宿主调度用) ----
    bool busy() const { return m_busy; }
    const std::string &taskId() const { return m_taskId; }
    // 这个任务对象绑定的工作目录(初始化时定下, 复用前要比较)
    const std::string &rootDir() const { return m_config.rootDir; }
    void setTaskId(const std::string &id) { m_taskId = id; }
    int64_t idleSinceMs() const { return m_idleSinceMs; }
    void markIdle(int64_t nowMs) { m_idleSinceMs = nowMs; }
    void markBusy() { m_idleSinceMs = 0; }
    JZAgent *agent() const { return m_agent; }
    JZVirtualTerminal *terminal() const { return m_terminal; }

    // 当前状态快照(SSE 新连接时推过去)
    Json::Value stateJson() const;

    // 网页删历史条目: 按 seq 删掉一条记录(在跑的那条也会删, 收尾时不再回填)
    bool removeHistory(int seq);
    bool dropRunningHistory();

    friend class JZAgentCli;

protected:
    void setupAgent();
    std::string buildSystemPrompt() const;
    std::string memoryFilePath() const;
    std::string buildMemoryBlock() const;
    std::string deriveVerifyCmd(const std::string &script, const std::string &explicitCmd) const;
    std::string resolveProgram(const std::vector<std::string> &cands, const std::string &fallback) const;
    std::string extractUserText(const Json::Value &params);
    std::vector<std::string> extractImages(const Json::Value &params, std::string &error);

    // 验证循环
    void runVerifyAsync(const std::string &cmd, int timeoutMs, const std::string &cwd,
                        const std::function<void(int, const std::string &)> &cb);
    void killVerify();
    void finishVerify(int exitCode);
    void onVerifyDone(int exitCode, const std::string &out);
    void onLoopFinished();

    // 收尾 / 取消 / 历史
    void respondTask(const std::string &status, int rounds, const std::string &reply,
                     const std::string &verifyOut, const std::string &err);
    void endLoop();
    void resetSession(bool cancelRunning);
    bool cancelLocalTask(const std::string &reason);
    void resetHistory();
    void pushHistory(const std::string &prompt, const std::string &verifyCmd);
    void finishHistory(const std::string &status, int rounds, const std::string &reply,
                       const std::string &verifyOut, const std::string &err);
    Json::Value recentJson() const;
    static int64_t nowMs();
    void onClientDestroyed(JZObject *obj);

    // JZAgent 信号
    void onReply(const std::string &text);
    void onThinking(const std::string &text);
    void onToolCall(const ToolCall &call);
    void onToolResult(const ToolCallResult &result);
    void onError(const std::string &err);
    void onFinished();

    // 出错自动重试
    enum RetryMode { RetryNone = 0, RetryTransient = 1, RetryFix = 2 };
    static RetryMode classifyError(JZAgent::AgentError code, const std::string &err);
    std::string buildRetryPrompt(RetryMode mode, JZAgent::AgentError code, const std::string &err) const;
    void scheduleErrorRetry(RetryMode mode);

    // 事件出口: 统一交给宿主(SSE / 控制面 / 日志), 任务自己不持有订阅者。
    void pushEvent(const std::string &type, const Json::Value &data);

    JZAgentCli *m_cli = nullptr;      // 宿主(通信出口; 不拥有)
    JZCliConfig m_config;             // 进程配置的副本, 任务自足
    JZCliToolHost m_host;
    JZAgent *m_agent = nullptr;
    JZVirtualTerminal *m_terminal = nullptr;
    JZNetworkAccessManager *m_netMan = nullptr;
    std::vector<JZAgentTool *> m_tools;
    JZMemoryTool *m_memory = nullptr;
    int m_systemPromptBytes = 0;

    // ---- 任务池状态 ----
    std::string m_taskId;             // 控制面任务 id; HTTP 任务为空
    int64_t m_idleSinceMs = 0;        // 空闲起点(ms); 0 = 忙/刚创建
    std::atomic<bool> m_busy{false};
    std::function<void(const Json::Value &)> m_respond;

    // ---- 单任务状态 ----
    std::atomic<bool> m_failed{false};
    std::atomic<bool> m_cancelled{false};
    std::string m_lastError;
    JZAgent::AgentError m_lastErrorCode = JZAgent::None;
    int m_errorRetries = 0;
    int m_maxErrorRetries = 0;
    int m_fixRetries = 0;
    int m_maxFixRetries = 0;
    int m_retryDelayMs = 1000;
    bool m_retryPending = false;
    int m_loopEpoch = 0;

    bool m_loopActive = false;
    int m_loopRound = 0;
    int m_maxLoopRounds = 10;
    std::string m_verifyCmd;
    int m_verifyTimeoutMs = 60000;
    std::string m_accumReply;
    std::string m_lastVerifyOutput;

    JZProcess *m_verifyProc = nullptr;
    bool m_verifyRunning = false;
    bool m_verifyTimedOut = false;
    int m_verifyGen = 0;
    std::string m_verifyOut;
    std::string m_verifyScript;
    std::function<void(int, const std::string &)> m_verifyCb;

    // 状态快照 / 日志
    std::string m_currentPrompt;
    std::string m_accumThink;
    bool m_thinkLogged = false;
    std::vector<JZCliToolSnap> m_toolSnaps;
    std::string m_lastFeedbackSig;
    int m_sameFailCount = 0;
    bool m_sessionStarted = false;

    // 每个任务自己的最近历史(宿主 /info 聚合各任务)
    std::vector<JZCliHistoryItem> m_history;
    int m_historySeq = 0;
    int m_dropHistorySeq = 0;   // 已被删掉的"运行中"条目: 收尾时跳过回填
    static const int kHistoryMax = 20;
    Json::Value m_lastResult;
    bool m_hasLastResult = false;
};

#endif // JZ_AGENT_CLI_TASK_H_
