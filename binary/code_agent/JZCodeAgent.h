#ifndef JZ_CODE_AGENT_H_
#define JZ_CODE_AGENT_H_

#include <string>
#include <vector>

#include "JZAgent.h"
#include "JZAgentTool.h"
#include "JZCodeTools.h"

// 代码助手自身的配置（与大模型配置 JZAgentConfig 分开）
class CodeAgentConfig
{
public:
    std::string workspace;      // 工作区根目录，所有文件操作都限制在此目录内
    std::string buildCommand;   // 默认编译命令
    std::string runCommand;     // 默认运行命令
    int buildTimeout = 10 * 60 * 1000;  // 编译超时(毫秒)
    int runTimeout = 60 * 1000;         // 运行超时(毫秒)
};

// 一条会话日志，网页端按 id 增量拉取
struct CodeAgentLog
{
    // user       用户提问
    // delta      大模型回复的文本片段(流式增量)
    // tool_call  工具被调用
    // tool_result 工具返回
    // error      出错
    // info       提示信息
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

    // 会话日志（id 单调递增，网页端用 since 做增量拉取）
    int logSeq() const;
    const std::vector<CodeAgentLog> &logs() const;
    JZAgentTokenUsage usage() const;

    const CodeAgentConfig &codeConfig() const;
    const JZAgentConfig &llmConfig() const;
    void setBuildCommand(const std::string &cmd);
    void setRunCommand(const std::string &cmd);
    bool setWorkspace(const std::string &path);
    // 更新大模型配置（保留会话日志，重建底层 agent）
    void applyLlmConfig(const JZAgentConfig &llm);

    // 文件浏览（网页端复用与工具一致的路径校验）
    bool listFiles(const std::string &rel, std::vector<std::string> &out, std::string &error) const;
    bool readFileText(const std::string &rel, std::string &text, std::string &error) const;

    const std::vector<std::string> &toolNames() const;

protected:
    void buildAgent();
    void abortTools();
    void addLog(const std::string &type, const std::string &text, const std::string &name = std::string());
    std::string buildSystemPrompt() const;

    void onReply(const std::string &text);
    void onError(const std::string &text);
    void onFinished();
    void onToolLog(const std::string &type, const std::string &name, const std::string &text);

    JZAgent *m_agent = nullptr;
    JZAgentConfig m_llmConfig;
    CodeAgentConfig m_codeConfig;

    std::vector<JZCodeTool *> m_tools;
    std::vector<std::string> m_toolNames;

    std::vector<CodeAgentLog> m_logs;
    int m_logId = 0;
    bool m_busy = false;
    bool m_stopping = false;
};

#endif // !JZ_CODE_AGENT_H_
