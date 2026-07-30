#ifndef JZ_AGENT_H_
#define JZ_AGENT_H_

#include <atomic>
#include <memory>

#include "JZNetworkAccessManager.h"
#include "JZOptional.h"
#include "JZResult.h"
#include "JZAgentTool.h"
#include "json/json.h"

struct SkillDefine
{
    std::string name;
    std::string tips;
    std::string usage;
};

class JZAgentTokenUsage
{
public:
    int prompt_tokens = 0;      // 本次输入token
    int completion_tokens = 0;  // 本次输出token
    int total_tokens = 0;       // 本次总计
};

// 历史消息：承载角色、内容、工具请求/返回等全部负载
class LLMHistory
{
public:
    enum Role
    {
        User,       // 用户
        Assistant,  // AI助手
        System,     // 系统提示
        Tool        // 工具返回
    };

    Role role;
    std::string content;
    std::vector<ToolCall> toolRequest;  // 工具请求
    std::string toolResponseId;         // 工具返回

    Json::Value toJson() const;
    bool fromJson(const Json::Value& root);

private:
    // 角色字符串 <-> 枚举 的通用转换，供序列化/反序列化复用
    static std::string roleToString(Role r);
    bool parseRole(const Json::Value& root);
};

struct LLMContext
{
    LLMContext();
    void clear();

    bool fromJson(const Json::Value& root);
    Json::Value toJson() const;

    std::vector<std::shared_ptr<LLMHistory>> history;
    JZAgentTokenUsage usage;
};

// 请求：承载类型、id 与对应负载
class JZAgentRequest
{
public:
    enum Type
    {
        SystemPrompt,
        UserPrompt,
        ToolCallResultPrompt,
        Stop,
    };

    JZAgentRequest(Type request_type, int id);

    int type;
    int id;

    std::string userPrompt;
    std::string systemPrompt;

    ToolCall toolCall;
    ToolCallResult toolCallResult;
};
typedef std::shared_ptr<JZAgentRequest> JZAgentRequestPtr;

class JZAgentConfig
{
public:
    std::string modelUrl;
    std::string modelName;
    std::string modelToken;

    std::string rootPath;
    int maxRounds = 100;
    int maxToolError = 3;   // 工具连续失败上限，超过则终止任务
};

class JZAgent : public JZObject
{
public:
    enum State{None, WaitingReply, Idle};

    JZAgent(JZObject* parent = nullptr);
    ~JZAgent();

    void init(JZAgentConfig config);
    
    const LLMContext* context() const;
    
    void setSystemPrompt(std::string prompt);
    std::string systemPrompt();
    std::string toolsPrompt();

    void addTool(JZAgentTool *tool);
    const JZAgentTool* tool(const std::string &name) const;

    bool streamEnabled() const;
    void setStreamEnabled(bool enabled);

    void transaction();  //暂时不提交
    void commit();       //提交  

    void systemRequest(std::string sys_prompt);
    void request(std::string user_prompt);  // 发送用户消息
    void stop();

    void addSkill(const SkillDefine& tool);
    const SkillDefine* skill(const std::string& name);
    JZResult<SkillDefine> loadSkill(std::string skill_name, const std::string& skill_path);

    std::string errorString();

jz_signals: 
    JZSigString sigReply;
    JZSigString sigError;
    JZSigVoid   sigFinished;

protected:
    struct LLMRequstInfo
    {
        int64_t requestTime;
    };

    std::string readFile(std::string filepath);

    // 生成一条请求（带递增 id）
    JZAgentRequestPtr makeAgentParam(JZAgentRequest::Type param_type);

    void clear();
    void llmRequest();
    void onLLMRequest();
    void onLLMStreamReadyRead();
    void processSSEEvent(const std::string& event);
    void processSSEData(const std::string& payload);

    // 统一的响应增量处理：流式(SSE 分块)与非流式(整包)共用同一条累积/回调路径
    void processResponseDelta(const std::string &content,
                              const Json::Value &tool_calls,
                              const Json::Value &usage);
    // 请求收尾：把累积的回复并入历史，并在需要工具时驱动工具调用
    void finalizeResponse();

    void onToolCallResult(const ToolCallResult &response);

    std::string buildSkillSummary();
    Json::Value buildContext();
    Json::Value buildTools();
    
    JZAgentTool* getTool(const std::string &name);
    void processToolCall(ToolCall &call);

    void llmLoop();
    void llmError(std::string reply);
    void llmEnd();

    std::string m_systemPrompt;
    std::vector<JZAgentTool*> m_tools;
    std::vector<SkillDefine> m_skills;
    LLMContext m_context;
    int m_toolError;

    bool m_inTransaction;

    std::vector<JZAgentRequestPtr> m_requests;
    bool m_isActive;
    int m_currentRound;
    State m_state;

    std::atomic<int> m_paramIdGen{0};
    
    JZNetworkReply* m_llmReply;
    LLMRequstInfo m_llmReqInfo;

    JZNetworkAccessManager* m_net;
    JZAgentConfig m_config;
    std::string m_error;
    bool m_streamEnabled;

    // 单次请求内累积的回复（流式与非流式统一使用）。
    // 单次 LLM 回复对应一条 assistant 历史，但用 vector 承载以便统一并入 m_context.history。
    // 流式过程中工具调用可能被分片，故只在响应完整后（[DONE]/整包）才执行。
    std::vector<std::shared_ptr<LLMHistory>> m_replyHistory;

    // 流式响应(SSE)原始文本累积缓冲
    std::string m_streamBuffer;
};

#endif
