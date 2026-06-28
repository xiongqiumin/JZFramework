#ifndef JZ_AGENT_H_
#define JZ_AGENT_H_

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

struct LLMHistory
{
    enum Role
    {
        User,       // 用户
        Assistant,  // AI助手
        System,     // 系统提示
        Tool        // 工具返回
    };

    Json::Value toJson() const;
    bool fromJson(const Json::Value& root);

    Role role;
    std::string content;
    std::vector<ToolCall> toolRequest;  // 工具请求
    std::string toolResponseId;         // 工具返回
};

struct LLMContext
{
    LLMContext();
    void clear();

    bool fromJson(const Json::Value& root);
    Json::Value toJson() const;

    vector<LLMHistory> history;
    JZAgentTokenUsage usage;
};

struct JZAgentRequest
{
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
JZAgentRequestPtr makeAgentParam(JZAgentRequest::Type type);

class JZAgentConfig
{
public:
    std::string modelUrl;
    std::string modelName;
    std::string modelToken;

    std::string rootPath;
    int maxRounds = 100;
};

class JZAgent : public JZObject
{
public:
    JZAgent(JZObject* parent = nullptr);
    ~JZAgent();

    const LLMContext* context() const;
    
    bool streamEnabled() const;
    void setStreamEnabled(bool enabled);

    void init(JZAgentConfig root_path);
    
    void setSystemPrompt(std::string prompt);
    std::string systemPrompt();
    std::string toolsPrompt();

    void transaction();  //暂时不提交
    void commit();       //提交  

    void systemRequest(std::string sys_prompt);
    void request(std::string user_prompt);  // 发送用户消息
    void stop();

    void addTool(const ToolDefine& tool);
    void toolCallResult(const ToolCallResult &response);
    const ToolDefine* tool(const std::string &name);

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

    void clear();
    void llmRequest();
    void onLLMRequest();

    std::string getContent(const Json::Value& value);

    std::string buildSkillSummary();
    Json::Value buildContext();
    Json::Value buildTools();
    
    ToolDefine *getTool(std::string name);
    void processToolCall(ToolCall &call);

    void llmLoop();
    void llmError(std::string reply);
    void llmEnd();

    std::string m_systemPrompt;
    std::vector<ToolDefine> m_tools;
    std::vector<SkillDefine> m_skills;
    LLMContext m_context;
    int m_toolError;

    bool m_inTransaction;

    std::vector<JZAgentRequestPtr> m_requests;
    bool m_isActive;
    int m_currentRound;
    
    JZNetworkReply* m_llmReply;
    LLMRequstInfo m_llmReqInfo;

    JZNetworkAccessManager* m_net;
    JZAgentConfig m_config;
    std::string m_error;
    bool m_streamEnabled;
};

#endif