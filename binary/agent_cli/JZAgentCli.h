#ifndef JZ_AGENT_CLI_H_
#define JZ_AGENT_CLI_H_

#include <atomic>
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
class JZHttpServer;
class JZHttpServerClient;
class JZHttpRequest;

// ---------------------------------------------------------------------------
// agent_cli 配置
//   与 agent_node 类似, 但多了 LLM 连接信息(modelUrl/Name/Token) —— 因为 agent_cli
//   自己就跑一个 JZAgent(直接驱动本地工具), 而不是把工具暴露给远端 agent 调。
// ---------------------------------------------------------------------------
struct JZCliConfig
{
    std::string agentName = "agent_cli";
    std::string version = "1.0.0";

    // ---- LLM ----
    std::string modelUrl;
    std::string modelName;
    std::string modelToken;
    int netTimeout = 120 * 1000;   // 网络请求超时(ms)
    int maxRounds = 100;           // 单任务最大对话轮次(-1=不限)
    int maxToken = 100000;         // 单任务 token 预算上限

    // ---- 工作区 / 工具 ----
    std::string rootDir;
    std::vector<TrustedDir> trustedDirs;

    bool allowTerminal = true;
    bool allowFileTools = true;
    bool allowPackageManager = false;
    bool allowNetwork = false;
    bool unrestricted = false;

    std::string shellProgram;
    int recycleSec = 15 * 60;
    std::string logFile;

    // 程序映射: 程序名 -> 可执行文件全路径(动态登记, 见 JZVirtualTerminalConfig::programPaths)
    //   典型是 tsc / code_runtime 这类"随部署走、通常不在 PATH 里"的程序 —— 登记后:
    //   进白名单(短名字能过审) + 执行时把命令里的短名换成全路径(短名字能找到程序)。
    //   ⚠ 只认短名字: 写全路径走路径审核(区外拒), 需要就另外把它配进 rootDir/trustedDirs。
    //   由外部(配置文件 terminal.programs / 宿主代码)传入; 没登记的程序 agent 就用不了。
    std::map<std::string, std::string> programPaths;

    // ---- HTTP 传输 ----
    std::string httpBind = "127.0.0.1";
    int httpPort = 8080;
    std::string httpToken;             // 非空则要求 ?token= 或 Authorization: Bearer(防裸奔)
    std::string httpDir;               // 网页静态目录; 空=exe 同目录/http
};

// ---------------------------------------------------------------------------
// JZCliToolHost —— agent_cli 侧的工具宿主
//   给 agent_tool 的通用工具喂区域配置 + 读文件(直读, 无缓存)。
//   与 agent_node 的 JZAgentNodeHost 同一套逻辑, 只是配置来自 JZCliConfig。
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
// JZAgentCli —— 通过 HTTP 暴露任务提交的 agent
//
// 传输: HTTP server。外部通过 POST /task 提交任务(JSON: prompt / verify_script /
// verify_cmd / verify_timeout / max_verify_rounds / token?), 本端跑 JZAgent, 每轮
// 结束跑验证脚本(返回 0 通过 / 1 失败), 失败把输出喂回 agent 自动续跑, 直到验证
// 通过或达最大轮次, 再一次性回 JSON {status, verify_rounds, agent_reply, ...}。
//
// 工具由本端**本地执行**: JZAgent 直接调用 agent_tool 里的工具(走 JZAgentTool 接口),
// 不经过 MCP。所以 agent_cli = "没有 UI 的 code_agent" + HTTP 传输层。
// ---------------------------------------------------------------------------
class JZAgentCli : public JZObject
{
public:
    explicit JZAgentCli(JZObject *parent = nullptr);
    ~JZAgentCli() override;

    // 建 JZAgent、注册工具、设 system prompt。必须在事件循环起来前调用。
    bool init(const JZCliConfig &config, std::string &error);

    // 起 HTTP 服务: 暴露 POST /task 提交任务 + 验证脚本门控循环。
    void startHttp();

    // /task 请求入口(由内部 TaskHandler 调用)
    void onTaskSubmit(JZHttpServerClient *client, const JZHttpRequest &req);

    // 网页控制台: 列出工作区内可当验证脚本的文件(GET /scripts)
    void onListScripts(JZHttpServerClient *client, const JZHttpRequest &req);

    // 网页控制台: 运行信息(GET /info) —— 顶栏显示模型地址等; SSE hello 帧带同一份数据
    //   {agent, version, modelUrl, modelName, rootDir, busy, round}。刻意不含 modelToken。
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
    //   type: hello / task_start / thinking / reply / tool_call / tool_result /
    //         error / verify / task_end / reset
    void onEvents(JZHttpServerClient *client, const JZHttpRequest &req);
    void pushEvent(const std::string &type, const Json::Value &data);   // 推给所有订阅者
    void removeEventClient(JZHttpServerClient *client);

    // 任务提交核心: 由 HTTP(onTaskSubmit) 调用, 解析 prompt / 验证脚本 / 轮次, 进入"提交→验证"循环。
    // respond 在循环结束(成功/失败/取消)时被调用一次, 带回 JSON 结果
    //   {status, verify_rounds, agent_reply, last_verify_output, error?}。
    void submitTask(const Json::Value &req, std::function<void(const Json::Value &)> respond);

    JZAgent *agent() const { return m_agent; }

    // 生效的程序映射(即配置里传进来的那份), 供宿主打印/自检
    const std::map<std::string, std::string> &programPaths() const { return m_config.programPaths; }

protected:
    void setupAgent();
    std::string buildSystemPrompt() const;

    std::string extractUserText(const Json::Value &params);

    // JZAgent 信号
    void onReply(const std::string &text);
    void onThinking(const std::string &text);
    void onToolCall(const ToolCall &call);
    void onToolResult(const ToolCallResult &result);
    void onError(const std::string &err);
    void onFinished();

    // 有没有 SSE 订阅者: 没有就不必构造高频的流式事件(thinking/reply)JSON
    bool hasEventConsumer() const { return !m_eventClients.empty(); }

    // HTTP 传输 + 验证脚本门控循环
    static std::string deriveVerifyCmd(const std::string &script, const std::string &explicitCmd);
    static void runVerify(const std::string &cmd, int timeoutMs, const std::string &cwd,
                         int &exitCode, std::string &output);
    void onLoopFinished();
    void respondTask(const std::string &status, int rounds, const std::string &reply,
                    const std::string &verifyOut, const std::string &err);
    bool checkToken(const JZHttpRequest &req) const;      // ?token= / Authorization: Bearer
    void resetSession(bool cancelRunning);                // 重置会话(可选先中止在途任务)
    void endLoop();
    void onClientDestroyed(JZObject *);
    void sendJson(JZHttpServerClient *client, int code, const Json::Value &j);

    JZCliConfig m_config;
    JZCliToolHost m_host;
    JZVirtualTerminal *m_terminal = nullptr;
    JZAgent *m_agent = nullptr;
    std::vector<JZAgentTool *> m_tools;

    std::atomic<bool> m_busy{false};
    std::atomic<bool> m_failed{false};
    std::atomic<bool> m_cancelled{false};
    std::string m_lastError;

    // HTTP 传输
    JZHttpServer *m_http = nullptr;
    JZHttpServerClient *m_pendingClient = nullptr;   // 当前在跑任务对应的 HTTP 连接(循环结束回包)
    bool m_loopActive = false;                       // 是否处于"提交→验证循环"模式
    int m_loopRound = 0;
    int m_maxLoopRounds = 10;
    std::string m_verifyCmd;                         // 推导好解释器的验证命令
    int m_verifyTimeoutMs = 60000;
    std::string m_accumReply;                        // 累积本轮 agent 最终回复
    std::string m_lastVerifyOutput;
    bool m_sessionStarted = false;                   // 是否已发起过首轮 request(避免首轮 reset 告警)

    // 传输无关扩展
    std::function<void(const Json::Value &)> m_respond;  // 循环结束的回包回调

    // SSE 订阅者(GET /events): 由连接析构回调摘除
    std::vector<JZHttpServerClient *> m_eventClients;
};

#endif // !JZ_AGENT_CLI_H_
