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
    int cached_tokens = 0;      // 本次从 KV 缓存命中的输入token（前缀缓存命中，计费通常折扣）
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

    // 消息性质：普通对话消息 / 上下文起点 / 压缩摘要。
    // - ContextStart: 当前上下文的**第一条 user 消息**(新对话的第一句话)。
    // - Summary:      历史超 token 预算被压缩时，旧对话整段被替换成的那条摘要。
    // 两者都是"上下文起点"(恢复时按最后一条非 Message 切片)，区别只在界面：
    // 用户第一句话要照常显示，而压缩摘要是内部消息、不进界面日志。
    // 转换见 kindToString / parseKind。
    enum Kind
    {
        Message,      // 普通对话消息
        ContextStart, // 上下文起点: 用户输入的第一条消息(界面照常显示)
        Summary,      // 压缩摘要(内部: 不进界面, 但同样是新的上下文起点)
    };

    Role role;
    Kind kind = Kind::Message;
    std::string content;
    std::string thinking;               // 推理/思考过程(reasoning_content/thinking), 仅实时上抛, 不回写模型上下文
    std::vector<std::string> images;    // 附带的图片(data URL), 多模态消息用
    std::vector<ToolCall> toolRequest;  // 工具请求
    std::string toolResponseId;         // 工具返回
    JZAgentTokenUsage usage;            // 本轮 token 增量（仅 Assistant 回复由响应 usage 填充）

    Json::Value toJson() const;
    bool fromJson(const Json::Value& root);

private:
    // 角色 / 消息性质 字符串 <-> 枚举 的通用转换，供序列化/反序列化复用
    static std::string roleToString(Role r);
    bool parseRole(const Json::Value& root);
    static std::string kindToString(Kind k);
    void parseKind(const Json::Value& root);
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
        TokenCompress, // 超出则压缩历史（把最旧一段历史发给大模型做摘要替换）
    };

    std::string rootPath;
    
    std::string modelUrl;
    std::string modelName;
    std::string modelToken;
    
    int netTimeout = 120 * 1000; // 网络请求超时（毫秒），默认 30s

    // 是否复用 TCP 连接(keep-alive)。true(默认): 复用, 多个请求共用同一条连接;
    // false: 每条请求带 `Connection: close`, 服务器收完即关, 客户端也不入池复用(用完即弃)。
    // 某些服务端(或代理)对 keep-alive 处理有异常、或需要每次新建连接时, 设为 false。
    bool reuseConnection = true;

    int maxRounds = 100;    // -1代表不限制
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

    // ---- 上下文存取（存储介质由调用方决定：文件 / SQLite / ...） ----
    // 导出当前上下文。systemPrompt / 工具 / 技能都不在里面, 由调用方重新设置。
    // 导出前会裁掉末尾不完整的一段(assistant 带 tool_calls 却没有对应的 tool 返回),
    // 否则恢复后把不一致的历史发给大模型会被拒绝; usage 按裁剪后的历史重算。
    // 无有效对话时返回的 ctx.history 为空。
    // 注意: 是**浅拷贝** —— history 里的消息对象与 agent 当前历史共享, 按只读使用。
    LLMContext saveContext() const;
    // 载入上下文(整体替换当前对话, 含 usage)。同样是**浅拷贝**(消息对象共享),
    // 传入的 ctx 之后不要再改动。
    // 与 saveContext 对称: 载入后同样裁到自洽, 防止脏存档发出不一致的历史。
    void loadContext(const LLMContext &ctx);

    void setSystemPrompt(std::string prompt);
    std::string systemPrompt();  
    
    std::string dumpSystemPrompt(); // system prompt + skills
    std::string dumpToolsPrompt();

    void addTool(JZAgentTool *tool);
    const JZAgentTool* tool(const std::string &name) const;

    // 接入一个 MCP 客户端: agent 接管 client 的所有权, 并把它的远端工具登记进来。
    // client 需先 startXxx() 成功(已连上并拉到工具列表); 与本地工具重名的远端工具会被跳过。
    // 代理工具本身仍归 client 所有, client 销毁时会自动从本 agent 的工具表中移除。
    void addMcp(JZMcpClient *client);

    bool streamEnabled() const;
    void setStreamEnabled(bool enabled);
    void setThinkEnabled(bool enabled);

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
    // 一条 user 消息附带多张图片(多模态): 每个元素为 http(s) 地址或 data URL, 原样作为
    // image_url.url 发送(顺序即发送顺序)。
    void request(const std::string &text, const std::vector<std::string> &image_urls);
    const std::vector<JZAgentRequestPtr> &requestList() const;
    void clearRequestList();

    void stop();

    // 一轮对话结束(Idle)后重开新任务: 把 agent 重置回 Inactive, 清空对话历史/用量/待发请求。
    // 保留已注册的 system prompt / tools / skills 配置, 重置后即可 setSystemPrompt/addTool 或直接 request。
    // 仅 Idle 状态允许; WaitingReply(请求在途)与 Inactive(无需重置)会被 jzWarning 拒绝。
    void reset();

    void addSkill(const SkillDefine& tool);
    const SkillDefine* skill(const std::string& name);
    JZResult<SkillDefine> loadSkill(std::string skill_name, const std::string& skill_path);

    std::string errorString();

jz_signals: 
    JZSigString sigReply;
    JZSigString sigThinking;   // 推理/思考增量(实时), 与 sigReply 并行
    JZSignal<const ToolCall&> sigToolCall;
    JZSignal<const ToolCallResult&> sigToolResult;
    JZSigString sigError;
    JZSigVoid   sigFinished;
    // 历史新增了一条消息(增量, 追加到末尾)。用于增量持久化: 接收方按序追加一条即可,
    // 不必等一轮结束再整段保存 —— 这样存档里能留下**完整**对话记录(含被压缩掉的旧消息),
    // 恢复时界面显示全部, 而给模型的上下文只取最后一条 ContextStart 之后的部分。
    // 压缩时发出的就是那条摘要本身(它既是新消息, 也是新的上下文起点)。
    // 参数是共享指针, 接收方可以长期持有。
    // 注: 只覆盖"追加"; 出错裁剪 / reset 清空导致的缩减不通知(接收方按需自行全量对齐)。
    // 跨线程(Queued/BlockingQueued)连接会把参数打包成 JZVariant, 自定义类型请用
    // 默认的 Auto(同线程即直连)/Direct 连接。
    JZSignal<std::shared_ptr<LLMHistory>> sigLLMHistory;

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

    // agent 不处于 Inactive 时拒绝修改会改变请求前缀的配置(system/tools/skills):
    // 运行期改动会让当前会话后续请求的前缀(system+tools)漂移,
    // 服务端 KV 前缀缓存整段失效, 已开始的历史全部作废。
    // 返回 true 表示已拒绝并打了 jzWarning, 调用方应直接 return。
    bool rejectWhileActive(const char *what) const;

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
                              const std::string &thinking,
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

    // 超出预算时把最旧一段历史发给大模型做摘要压缩，用一条摘要消息替换原消息。
    // 成功发起异步压缩返回 true；无可压缩项(策略不支持/已无可压)返回 false。
    bool startCompressHistory();
    void onCompressFinished();

    void llmError(AgentError error, std::string error_string = std::string());
    void llmEnd();

    std::string m_systemPrompt;
    std::vector<JZAgentTool*> m_tools;
    std::vector<McpEntry> m_mcpEntries;
    std::vector<SkillDefine> m_skills;
    std::atomic<int> m_paramIdGen{ 0 };

    bool m_streamEnabled;  //是否流式传输
    bool m_thinkEnabled;   //是否思考

    JZAgentTool *m_readSkill;
    LLMContext m_context;
    int m_toolError;

    bool m_inTransaction;

    std::vector<JZAgentRequestPtr> m_requests;
    bool m_isActive;
    int m_currentRound;  //单词llm 请求的round
    State m_state;
    
    JZNetworkReply* m_llmReply;
    LLMRequstInfo m_llmReqInfo;

    // 历史压缩(TokenCompress)相关的在途请求与临时状态
    JZNetworkReply* m_compressReply = nullptr;
    std::vector<std::shared_ptr<LLMHistory>> m_compressChunk;  // 本次被压缩的历史块

    JZNetworkAccessManager* m_net;
    JZAgentConfig m_config;
    std::string m_error;
    AgentError m_errorCode = AgentError::None;  // 未设置错误时为 None

    // 单次请求内累积的回复（流式与非流式统一使用）。
    // 单次 LLM 回复对应一条 assistant 历史，流式过程中工具调用可能被分片，
    // 故只在响应完整后（[DONE]/整包）才执行。
    std::shared_ptr<LLMHistory> m_replyHistory;
    std::map<std::string,JZAgentTool*> m_pendingTools;
};

#endif
