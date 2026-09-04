#ifndef JZ_AGENT_H_
#define JZ_AGENT_H_

#include <atomic>
#include <memory>

#include "JZNetworkAccessManager.h"
#include "JZByteArray.h"
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
    std::vector<std::string> images;    // 附带的图片(data URL), 多模态消息用
    std::vector<ToolCall> toolRequest;  // 工具请求
    std::string toolResponseId;         // 工具返回
    JZAgentTokenUsage usage;            // 本轮 token 增量（仅 Assistant 回复由响应 usage 填充）

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

    // 根据 history 中每条历史的 usage（增量）重算累计 usage
    void recalcUsage();

    std::vector<std::shared_ptr<LLMHistory>> history;
    JZAgentTokenUsage usage;  // 累计 token = sum(history[i].usage)
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
    std::vector<std::string> images;   // 随本条 user 消息一起发送的图片(data URL)

    ToolCall toolCall;
    ToolCallResult toolCallResult;
};
typedef std::shared_ptr<JZAgentRequest> JZAgentRequestPtr;

class JZAgentConfig
{
public:
    enum TokenStrategy {
        TokenError,    // 超出预算直接报错终止
        TokenTruncate, // 超出则裁剪最旧历史后重试
        TokenCompress, // 超出则压缩历史（暂未实现）
    };

    std::string rootPath;
    
    std::string modelUrl;
    std::string modelName;
    std::string modelToken;
    
    int netTimeout = 30000; // 网络请求超时（毫秒），默认 30s

    int maxRounds = 100;
    int maxToolError = 3;   // 工具连续失败上限，超过则终止任务
    int maxToken = 100000;  // 单次任务 token 预算上限，超出触发 maxTokenError

    TokenStrategy tokenStrategy = TokenError;

    // 附加到聊天请求体上的自定义字段, 用于对接各家私有参数
    // (如 vLLM 的 chat_template_kwargs={"enable_thinking":false}、max_tokens 等),
    // 与框架自己生成的字段同名时以本字段为准。
    Json::Value extraBody;
};

class JZMcpClient;

class JZAgent : public JZObject
{
public:
    enum State{Inactive, WaitingReply, Idle};

    // Agent 错误分类：在 JZNetworkReply::NetworkError 基础上补充 ToolCallError
    enum AgentError {
        None,
        NoError,
        ConnectError,
        HttpError,
        TimeOutError,
        ToolCallError,
        UserCancelError,
        MaxRoundError,   // 达到最大对话轮次
        MaxTokenError,   // token 预算超出
    };

    JZAgent(JZObject* parent = nullptr);
    ~JZAgent();

    void init(JZAgentConfig config);
    
    const LLMContext* context() const;

    // 只持久化对话历史: systemPrompt / 工具 / 技能都不写进文件, 由调用方重新设置。
    // 保存前会裁掉末尾不完整的一段(assistant 带 tool_calls 却没有对应的 tool 返回),
    // 否则恢复后把不一致的历史发给大模型会被拒绝。
    bool saveHistory(const std::string &filepath) const;
    bool loadHistory(const std::string &filepath);

    void setSystemPrompt(std::string prompt);
    std::string systemPrompt();
    std::string toolsPrompt();

    void addTool(JZAgentTool *tool);
    const JZAgentTool* tool(const std::string &name) const;

    // 接入一个 MCP 客户端: agent 接管 client 的所有权, 并把它的远端工具登记进来。
    // client 需先 startXxx() 成功(已连上并拉到工具列表); 与本地工具重名的远端工具会被跳过。
    // 代理工具本身仍归 client 所有, client 销毁时会自动从本 agent 的工具表中移除。
    void addMcp(JZMcpClient *client);

    bool streamEnabled() const;
    void setStreamEnabled(bool enabled);

    void transaction();  //暂时不提交
    void commit();       //提交  

    void systemRequest(std::string sys_prompt);
    void request(std::string user_prompt);  // 发送用户消息

    // 发送带一张图片的用户消息(多模态), 图片与文字在同一条 user 消息里:
    //   text + image_url   ->  一条消息, 而不是两条
    //   image_url  : 图片地址(http://... 或 data:...), 原样作为 image_url.url 发送
    //   image_base64: 图片的 base64 文本, mime 按内容自动识别后拼成 data URL
    // 注意: 历史里保存的就是拼好的 data URL, 每轮重发的字节完全一致, 利于服务端前缀缓存。
    void request(const std::string &text, const std::string &image_url);
    void request(const std::string &text, const JZByteArray &image_base64);

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

    // 一个 MCP 客户端及其登记进来的代理工具(工具归 client 所有)
    struct McpEntry
    {
        JZMcpClient *client = nullptr;
        std::vector<JZAgentTool*> tools;
    };

    void onMcpDestroyed(JZObject *obj);

    std::string readFile(std::string filepath);

    // 生成一条请求（带递增 id）
    JZAgentRequestPtr makeAgentParam(JZAgentRequest::Type param_type);

    // 带图片的 user 消息: images 为拼好的 data URL
    void requestWithImages(const std::string &text, const std::vector<std::string> &images);

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
    void llmError(AgentError error, std::string error_string = std::string());
    void llmEnd();

    std::string m_systemPrompt;
    std::vector<JZAgentTool*> m_tools;
    std::vector<McpEntry> m_mcpEntries;
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
    AgentError m_errorCode = AgentError::None;  // 未设置错误时为 None
    bool m_streamEnabled;

    // 单次请求内累积的回复（流式与非流式统一使用）。
    // 单次 LLM 回复对应一条 assistant 历史，流式过程中工具调用可能被分片，
    // 故只在响应完整后（[DONE]/整包）才执行。
    std::shared_ptr<LLMHistory> m_replyHistory;
};

#endif
