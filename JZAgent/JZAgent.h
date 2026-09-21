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

// 单轮(或多轮累计)的 token 用量。
// 同时兼容两家写法：
//   - OpenAI: {prompt_tokens, completion_tokens, total_tokens, prompt_tokens_details.cached_tokens}
//   - llama.cpp: {prompt_n, predicted_n, cache_n} + 计时对象 timings
//     {prompt_ms, prompt_per_token_ms, prompt_per_second, predicted_ms, predicted_per_token_ms,
//      predicted_per_second, draft_n, draft_n_accepted}
// 计时类只存"原始量"(token 数与毫秒)，速率一律现算 —— 速率求和无意义，
// 多轮累计只能按 Σtoken / Σms 算平均速率(见 promptTokensPerSecond / predictedTokensPerSecond)。
class JZAgentTokenUsage
{
public:
    int prompt_tokens = 0;      // 本次输入token
    int completion_tokens = 0;  // 本次输出token
    int total_tokens = 0;       // 本次总计
    int cached_tokens = 0;      // 本次从 KV 缓存命中的输入token（前缀缓存命中，计费通常折扣）

    // ---- llama.cpp timings（其他 provider 不带就是 0）----
    double prompt_ms = 0;           // 本次 prefill(输入) 耗时
    double predicted_ms = 0;        // 本次生成(输出) 耗时
    int draft_tokens = 0;           // 投机采样/MTP 草稿 token 数(llama.cpp draft_n)
    int draft_accepted_tokens = 0;  // 投机采样/MTP 被接受数(llama.cpp draft_n_accepted)

    bool hasTimings() const
    {
        return prompt_ms > 0 || predicted_ms > 0 || draft_tokens > 0;
    }
    // prefill 速度(token/s)；无耗时返回 0
    double promptTokensPerSecond() const
    {
        return prompt_ms > 0 ? prompt_tokens * 1000.0 / prompt_ms : 0.0;
    }
    // 生成速度(token/s)；无耗时返回 0
    double predictedTokensPerSecond() const
    {
        return predicted_ms > 0 ? completion_tokens * 1000.0 / predicted_ms : 0.0;
    }
    // 投机采样接受率 [0,1]；无草稿返回 0
    double draftAcceptRate() const
    {
        return draft_tokens > 0 ? double(draft_accepted_tokens) / double(draft_tokens) : 0.0;
    }
    // 前缀缓存命中率 [0,1]（相对输入 token）
    double cacheHitRate() const
    {
        return prompt_tokens > 0 ? double(cached_tokens) / double(prompt_tokens) : 0.0;
    }
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
    // 本条消息所属**那一轮请求**的 token 用量(仅 Assistant 回复由响应 usage 填充)。
    // ⚠ prompt_tokens 是那轮的**整段 prompt**(prompt_n + 命中的 cache_n)，不是这条消息自身
    // 的 token 数 —— 所以它不能逐条相加当"上下文大小"用(见 JZAgent::windowTokens())。
    JZAgentTokenUsage usage;

    Json::Value toJson() const;
    bool fromJson(const Json::Value& root);

    // 把响应里的 usage 并入出参：兼容 OpenAI 标准写法与 llama.cpp(timings/cache_n/prompt_n)，
    // 只覆盖源里出现的字段(流式分批下发时不会被清零)。流式/非流式共用。
    static void mergeUsageJson(const Json::Value& src, JZAgentTokenUsage& usage);

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

    // 根据 history 中每条历史的 usage（各轮请求用量）重算累计 usage
    void recalcUsage();

    std::vector<std::shared_ptr<LLMHistory>> history;
    // 会话累计消耗 = Σ history[i].usage（计费口径；同一段历史会在每一轮的 prompt 里各计一次，
    // **不是**当前上下文大小 —— 要上下文大小用 JZAgent::windowTokens()）。
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
    // 上下文预算上限：按 JZAgent::windowTokens()（下一次请求实际会发出的上下文估算）判定，
    // 超出按 tokenStrategy 处理(error / compress)。应与模型的 n_ctx 对齐并留出生成余量。
    int maxToken = 100000;

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

    // 下一次请求实际要发出的上下文大小(token 估算) —— 预算判定与界面用量环都该用它。
    //   ⚠ 别拿 context()->usage.total_tokens 当"上下文": 那是 Σ history[i].usage, 而每条
    //   assistant 的 usage 记的是**那一轮整段 prompt**(prompt_n + cache_n, 见 mergeUsageJson),
    //   累加等于把同一段历史重复计入轮数遍(实测能到真实值的 4~6 倍)，预算会被提前顶爆。
    int windowTokens() const;

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

    // 事务(批量提交): transaction() 之后提交的多条消息先入队不发送, commit() 时
    // 合并成一次请求发出(多条消息进同一轮)。与"运行期禁止提交"是两回事:
    // 事务期内只入队不算提交, 真正的提交点是 commit(), 故不受 rejectWhileRunning 约束。
    void transaction();  //暂时不提交
    void commit();       //提交

    // 提交一条消息。注: agent 运行中(一轮对话尚未结束)不允许提交, 会被拒绝并 jzWarning,
    // 不再缓存起来等下一轮发出 —— 请先 stop() 或等 sigFinished 后再提交。
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

    void compressHistory();

    void stop();

    // 一轮对话结束(Idle)后重开新任务: 把 agent 重置回 Inactive, 清空对话历史/用量/待发请求。
    // 保留已注册的 system prompt / tools / skills 配置, 重置后即可 setSystemPrompt/addTool 或直接 request。
    // 仅 Idle 状态允许; WaitingReply(请求在途)与 Inactive(无需重置)会被 jzWarning 拒绝。
    void reset();

    void addSkill(const SkillDefine& tool);
    const SkillDefine* skill(const std::string& name);
    JZResult<SkillDefine> loadSkill(std::string skill_name, const std::string& skill_path);

    std::string errorString();

    // 最近一次错误的分类(见 AgentError)。llmError() 里"先置分类, 再发 sigError",
    // 所以 sigError 回调里读到的就是本次错误的分类; 供调用方决定值不值得重试
    // (连接失败/超时/5xx 值得; 用户取消/超轮次/超 token 重试也没用)。
    AgentError errorCode() const { return m_errorCode; }

jz_signals: 
    JZSigString sigReply;
    JZSigString sigThinking;   // 推理/思考增量(实时), 与 sigReply 并行
    JZSignal<const ToolCall&> sigToolCall;
    JZSignal<const ToolCallResult&> sigToolResult;
    JZSigString sigError;
    JZSigVoid   sigFinished;
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

    // agent 正在运行(一轮对话已开始且尚未结束)时拒绝提交新消息:
    // 运行期提交不会被缓存, 直接丢弃并 jzWarning(调用方要么 stop(), 要么等 sigFinished)。
    // 事务期(transaction 后 / commit 前)只是入队, 不算提交, 故不拦截。
    // 返回 true 表示已拒绝并打了 jzWarning, 调用方应直接 return。
    bool rejectWhileRunning(const char *what) const;

    std::string readFile(std::string filepath);

    // 生成一条请求（带递增 id）
    JZAgentRequestPtr makeAgentParam(JZAgentRequest::Type param_type);

    // 带图片的 user 消息: images 为拼好的 data URL
    void requestWithImages(const std::string &text, const std::vector<std::string> &images);

    // 入队一条请求: 事务期进 m_transactionRequests(不发送), 否则进 m_requests 并驱动 llmLoop
    void pushRequest(const JZAgentRequestPtr &req);

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

    std::string buildSkillSummary() const;
    Json::Value buildContext();
    Json::Value buildTools() const;
    
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

    // 配置项(不是运行态): 只由 setStreamEnabled/setThinkEnabled 改, reset() 不该动它们 ——
    // 否则"清空历史重开一个任务"会把流式/思考设置一起抹掉(第二个任务起悄悄退回非流式)。
    bool m_streamEnabled = false;  //是否流式传输
    bool m_thinkEnabled = true;    //是否思考

    JZAgentTool *m_readSkill;
    LLMContext m_context;

    bool m_inTransaction;   //事物,用于一次提交多组

    std::vector<JZAgentRequestPtr> m_requests;             // 常规请求队列(提交即发出)
    std::vector<JZAgentRequestPtr> m_transactionRequests;  // 事务期暂存的批次, commit() 时整批搬入 m_requests
    bool m_isActive;
    int m_currentRound;  //单词llm 请求的round
    State m_state;
    
    JZNetworkReply* m_llmReply;
    LLMRequstInfo m_llmReqInfo;

    // 历史压缩(TokenCompress)相关的在途请求与临时状态
    JZNetworkReply* m_compressReply = nullptr;

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
