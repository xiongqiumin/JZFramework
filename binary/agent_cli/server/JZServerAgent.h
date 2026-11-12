#ifndef JZ_SERVER_AGENT_H_
#define JZ_SERVER_AGENT_H_

// ---------------------------------------------------------------------------
// JZServerAgent —— server 进程**自己**跑的 JZAgent(内置 agent)。
//
// 为什么 server 也要有 agent: 管理台需要"能操作这个集群"的角色 —— 看哪些 cli 在线、
// 给某台派任务、查任务记录、启停本地子进程。这些事只有 server 知道, 远端 cli 的
// 工具表里没有(也不该有: 它们的权限边界在自己的工作区)。
//
// 所以它出现在 /api/status 的 online 列表里, agentId 默认 "server", 与远端 cli 平级:
// 网页点进去、新建任务、看记录, 走的是同一套接口; 区别只是任务在 server 进程内跑,
// 而不是下发到 hub 连接。
//
// 工具: 本机终端(terminal_*) + 文件读取 + server 独有工具(见 cpp 里的 *Tool 类)。
// 不做的: 它不参与 verify 门控(server 本机没有"产物"概念, 任务结束即结束)。
// ---------------------------------------------------------------------------

#include <memory>
#include <string>

#include "JZObject.h"
#include "JZSignal.h"
#include "json/json.h"

class JZAgent;
class JZVirtualTerminal;
class JZAgentCliServer;
class JZServerAgentToolHost;

struct JZServerAgentConfig
{
    bool enabled = false;                 // 要不要起(配置 selfAgent.enabled)
    std::string agentId = "server";       // 网页/API 里的 id
    std::string name = "server(内置)";
    std::string rootDir;                  // 本机工作区(空 = server 同目录)
    std::string modelUrl;
    std::string modelName;
    std::string modelToken;
    int netTimeout = 120000;
    int maxRounds = 40;
    int maxToken = 100000;
    std::string systemExtra;              // 宿主追加说明(可选)
    std::string skillsDir;                // 全局 .jzagent 目录(空 = %USERPROFILE%/.jzagent)
};

// 一条事件: type = reply / tool_call / tool_result / done / error
struct JZServerAgentEvent
{
    std::string taskId;
    std::string type;
    std::string data;   // JSON 字符串(文本类事件就是 JSON string, 省得前端判类型)
};

class JZServerAgent : public JZObject
{
public:
    explicit JZServerAgent(JZObject *parent = nullptr);
    ~JZServerAgent() override;

    bool init(const JZServerAgentConfig &cfg, JZAgentCliServer *srv, std::string &error);

    const JZServerAgentConfig &config() const { return m_cfg; }
    // 网页上切换工作目录: 终端/工具/系统提示都绑在目录上, 只能整体重建。
    // 正忙时拒绝(等任务结束再切)。目录不变则直接成功。
    bool setRootDir(const std::string &dir, std::string &error);
    bool busy() const { return m_busy; }
    int round() const { return m_round; }
    const std::string &taskId() const { return m_taskId; }
    const std::string &lastEventType() const { return m_lastEvent; }

    // 提交一条任务(同步返回, 结果经 sigEvent(type="done") 异步回到宿主)
    // ctxJson: 续会话用。上一条任务存的 LLMContext.toJson()(跟任务记录一起落盘),
    // 新任务照常 reset() 之后把它 loadContext 灌回去 —— reset() 依旧彻底清空。
    // 空 = 新会话。
    void submit(const std::string &taskId, const std::string &prompt, const std::string &verifyCmd,
                const Json::Value &ctxJson = Json::Value());
    // 上一轮结束时的对话快照: server 在 done 时存进任务记录(续会话靠它, 本类不缓存)
    const Json::Value &lastContextJson() const { return m_lastCtxJson; }
    // 已加载的 skill 名(项目 .jzagent/skills + 全局 ~/.jzagent/skills)
    std::vector<std::string> skillNames() const;
    void cancel();

jz_signals:
    JZSignal<JZServerAgentEvent> sigEvent;

private:
    void onReply(const std::string &delta);
    void onThinking(const std::string &delta);
    void onToolCall(const struct ToolCall &call);
    void onToolResult(const struct ToolCallResult &res);
    void onError(const std::string &err);
    void onFinished();
    void emitEvent(const std::string &type, const std::string &data);
    std::string buildSystemPrompt() const;
    // 按当前 m_cfg 建 agent/终端/工具/提示词(init 与切换目录共用)
    bool build(std::string &error);

    JZServerAgentConfig m_cfg;
    JZAgentCliServer *m_srv = nullptr;
    JZAgent *m_agent = nullptr;
    JZVirtualTerminal *m_terminal = nullptr;
    std::unique_ptr<JZServerAgentToolHost> m_toolHost;

    bool m_busy = false;
    bool m_everRan = false;     // 第二次任务前要 reset(仅 Idle 允许)
    Json::Value m_lastCtxJson;  // 上一轮结束时的对话快照(存档由任务记录负责, 这里只留最近一份)
    bool m_failed = false;
    bool m_cancelled = false;
    int m_round = 0;
    std::string m_taskId;
    std::string m_reply;
    std::string m_error;
    std::string m_lastEvent;
};

#endif // !JZ_SERVER_AGENT_H_
