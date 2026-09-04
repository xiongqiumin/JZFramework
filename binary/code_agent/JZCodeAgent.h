#ifndef JZ_CODE_AGENT_H_
#define JZ_CODE_AGENT_H_

#include <deque>
#include <map>
#include <string>
#include <vector>

#include "JZAgent.h"
#include "JZAgentTool.h"
#include "JZCodeTools.h"

// 代码助手自身的配置（与大模型配置 JZAgentConfig 分开）
class CodeAgentConfig
{
public:
    std::string workspace;                    // 工作区根目录，可读写
    std::vector<TrustedDir> trustedDirs;     // 授信目录（编译器/SDK 等），默认只读，可单独标记可写
    std::vector<TrustedProgram> trustedPrograms;  // 显式指定的可信程序（全路径），优先于目录搜索

    std::string webSearchUrl;                // 网络搜索接口模板(含 {q} 占位符)；空则用内置默认
    // HTTP 代理（仅对 http 目标生效；https 目标直连，不走代理）
    std::string proxyHost;
    int proxyPort = 0;
    std::string proxyUser;
    std::string proxyPassword;
};

// 单任务熔断配置
//   定位: 这类保护是 code_agent 这个应用的策略，不进入 JZAgent 通用框架。
//   tool 层只负责把"被拒绝"的原因结构化上报，统计与熔断由 JZCodeAgent 完成。
struct GuardConfig
{
    int maxToolCalls = 50;      // 单条指令内工具调用总次数上限
    int maxDurationSec = 900;   // 单条指令墙钟时长上限(秒)
    int maxSameDeny = 3;        // 同一类拒绝原因的累计次数上限
    int maxRepeatCall = 3;      // 完全相同的调用连续出现次数上限(判定陷入重复)
};

// 单任务熔断统计，每次用户下发新指令时重置
struct GuardState
{
    int toolCalls = 0;
    int totalDeny = 0;
    int64_t startTimeMs = 0;
    std::map<int, int> denyCount;            // DenyReason -> 次数
    std::deque<std::string> recentCalls;     // 最近的调用指纹(工具名 + 参数)
};

// 一条会话日志，网页端按 id 增量拉取
struct CodeAgentLog
{
    // user         用户提问
    // thinking     思考过程片段(流式增量, 实时上抛, 不入历史文件)
    // delta        大模型回复的文本片段(流式增量)
    // assistant    整条助手回复(历史重建用)
    // tool_call    工具被调用
    // tool_result  工具返回
    // error        出错
    // info         提示信息
    std::string type;
    std::string name;   // 工具名
    std::string text;
    int id = 0;

    Json::Value toJson() const;
};

// 代码助手：把 JZAgent 与一组代码工具(编译/运行/改代码/找代码)组装起来，
// 并维护一份可供网页端增量拉取的会话日志。
class JZCodeAgent : public JZObject
{
public:
    JZCodeAgent(JZObject *parent = nullptr);
    ~JZCodeAgent();

    void init(const JZAgentConfig &llm, const CodeAgentConfig &code);
    void request(const std::string &text);
    void stop();
    void clearHistory();
    bool isBusy() const;

    // 是否有指令在排队/刚续发仍等待回复(供界面显示"等待回复中...")
    bool waitingReply() const;

    // 会话日志（id 单调递增，网页端用 since 做增量拉取）
    int logSeq() const;
    const std::vector<CodeAgentLog> &logs() const;
    JZAgentTokenUsage usage() const;

    const CodeAgentConfig &codeConfig() const;
    const JZAgentConfig &llmConfig() const;
    bool setWorkspace(const std::string &path);

    // 熔断配置（长时间跑/反复被拦时的保护）
    void setGuardConfig(const GuardConfig &cfg);
    const GuardConfig &guardConfig() const;

    // 更新大模型配置（保留会话日志，重建底层 agent）
    void applyLlmConfig(const JZAgentConfig &llm);

    // 工作区对话存档（<工作区>/.jzcode）：切换工作区时自动保存旧对话、导入新对话。
    // 只存对话历史，systemPrompt / 工具定义每次重建时重新生成，不进文件。
    std::string historyFilePath() const;
    void saveWorkspaceHistory();
    void loadWorkspaceHistory();
    void rebuildLogsFromHistory();

    // 文件浏览（网页端复用与工具一致的路径校验）
    bool listFiles(const std::string &rel, std::vector<std::string> &out, std::string &error) const;
    bool readFileText(const std::string &rel, std::string &text, std::string &error) const;

    const std::vector<std::string> &toolNames() const;

protected:
    void buildAgent();
    void abortTools();
    // 把当前配置转成虚拟终端的区域配置（工作区 + 授信目录）
    void applyZoneConfig();
    void addLog(const std::string &type, const std::string &text, const std::string &name = std::string());
    std::string buildSystemPrompt() const;

    // 熔断: 每次下发新指令时重置，工具调用/拒绝时累计，超阈值则停止当前任务
    void resetGuard();
    void checkGuard();
    void triggerGuard(const std::string &reason, const std::string &suggestion);

    void onReply(const std::string &text);
    void onThinking(const std::string &text);
    void onError(const std::string &text);
    void onFinished();
    void onToolLog(const std::string &type, const std::string &name, const std::string &text, int deny);

    JZAgent *m_agent = nullptr;
    JZAgentConfig m_llmConfig;
    CodeAgentConfig m_codeConfig;

    std::vector<JZCodeTool *> m_tools;
    std::vector<std::string> m_toolNames;

    // 所有网络工具（web_search / web_fetch）共享的访问管理器，代理只在此统一设置一次
    JZNetworkAccessManager *m_netMan = nullptr;

    std::vector<CodeAgentLog> m_logs;
    int m_logId = 0;
    bool m_busy = false;
    bool m_stopping = false;

    GuardConfig m_guardConfig;
    GuardState m_guard;

    // 运行期间新到的指令缓存(自管队列, 不依赖 JZAgent 的 m_requests)，
    // 等上一步(当前任务)发完后由 onFinished 取出继续发送。
    std::deque<std::string> m_pendingQueue;
    bool m_waiting = false;      // 队列有缓存 或 刚把缓存发出还在等回复
};

#endif // !JZ_CODE_AGENT_H_
