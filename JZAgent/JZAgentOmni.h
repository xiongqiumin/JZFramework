#ifndef JZ_AGENT_OMNI_H_
#define JZ_AGENT_OMNI_H_
#include <string>
#include <vector>

#include "JZObject.h"
#include "json/json.h"

// ---------------------------------------------------------------------------
// JZAgentOmni —— 标准网络协议(OpenAI chat/completions) 与本地模型(单串 prompt)
// 之间的**纯文本适配层**。不加载模型、不推理、不起线程、不碰网络。
//
//   请求方向: buildPrompt(标准请求 JSON) -> 本地模型要的一整段 prompt 字符串
//   响应方向: parseResponse(本地模型原始文本) -> 标准响应 JSON
//             parseDelta(原始文本分片)        -> 标准流式 chunk JSON
//
// 对齐 JZAgent 现有的提交/接受格式:
//   请求 {model, messages[], tools[], tool_choice, chat_template_kwargs, ...}
//         messages 支持 role = system / user / assistant / tool;
//         content 可以是字符串, 也可以是 [{type:"text"},{type:"image_url"}];
//         assistant 可带 tool_calls, tool 消息带 tool_call_id;
//         role = thinking / error 是本框架的存档专用消息, 直接跳过。
//   响应 {id, object, created, model,
//         choices:[{index, message:{role, content, reasoning_content, tool_calls},
//                   finish_reason}], usage:{...}}
//
// 解析范围(本地模型的原始输出):
//   - <|im_end|> / <|endoftext|> / <|audio_pad|> 之类特殊 token: 一律剥掉;
//   - <think>...</think>: 抽成 reasoning_content, 不进 content;
//   - <tool_call>{...}</tool_call>: 抽成 tool_calls(JSON 解析失败则退回正文);
//   - <function=name>[<parameter=k>v</parameter>...]</function>: 同上;
//   - 标签被分片切断 / 输出被截断(只有开标签): 缓冲到齐或按正文降级, 不抛错。
// ---------------------------------------------------------------------------

struct JZFRAMEWORK_API JZAgentOmniConfig
{
    // ChatML 模板(与 Qwen 系一致), 换模型只改这里
    std::string systemStart = "<|im_start|>system\n";
    std::string userStart = "<|im_start|>user\n";
    std::string assistantStart = "<|im_start|>assistant\n";
    std::string turnEnd = "<|im_end|>\n";

    // 思考块
    bool enableThinking = false;          // false: prompt 里预闭合一个空的思考块
    std::string thinkStart = "<think>\n";
    std::string thinkEnd = "</think>\n\n";

    // 工具: 本地模型没有原生 tools 接口, 默认把定义以 JSON 塞进 system
    bool inlineTools = true;
    std::string toolsTitle = "\n# Tools\n";
    std::string toolCallStart = "<tool_call>";
    std::string toolCallEnd = "</tool_call>";
    std::string toolResponseStart = "<tool_response>\n";
    std::string toolResponseEnd = "\n</tool_response>";

    // 多模态占位: content 里的 image_url 换成标记, 真正的字节由调用方另行喂给模型
    std::string imageMarker = "<|vision_start|><|image_pad|><|vision_end|>";
    std::string audioMarker = "<|audio_start|><|audio_pad|><|audio_end|>";
};

class JZFRAMEWORK_API JZAgentOmni : public JZObject
{
    JZ_OBJECT_DECLARE(JZAgentOmni)

public:
    // token 用量: 本地引擎算出来的数字, 由调用方带进来(解析器自己不数)
    struct Usage
    {
        int prompt_tokens = 0;
        int completion_tokens = 0;
    };

    explicit JZAgentOmni(JZObject *parent = nullptr);
    ~JZAgentOmni();

    void setConfig(const JZAgentOmniConfig &config);
    const JZAgentOmniConfig &config() const;

    // ---- 请求方向 ----
    // 标准请求 -> prompt。fromIndex: 从 messages 的第几条开始拼
    //   0  = 全量重放(KV 为空 / 刚恢复会话, system 也只在这时拼);
    //   >0 = 只拼新增的轮次(本地 KV 里已经有前面那些)。
    // thinking 取值优先级: request.chat_template_kwargs.enable_thinking > config.enableThinking
    std::string buildPrompt(const Json::Value &request, int fromIndex = 0) const;
    // 只给 messages 和 tools 的底层入口(tools 可为 null)。
    std::string buildPromptFromMessages(const Json::Value &messages, const Json::Value &tools,
                                        bool enableThinking, int fromIndex = 0) const;

    // ---- 响应方向 ----
    // 原始输出 -> 标准响应(choices[0].message + usage)。
    Json::Value parseResponse(const std::string &raw, const std::string &model = std::string(),
                              const Usage &usage = Usage());

    // 流式: beginStream() 开一轮 -> parseDelta(分片) 若干次 -> endStream() 收尾。
    // 分片可以在标签中间切开; 每片返回一个标准 chunk(delta.content / delta.reasoning_content),
    // 工具调用在 endStream() 的那一片里整批给出(不按分片切)。
    void beginStream();
    Json::Value parseDelta(const std::string &piece);
    Json::Value endStream(const std::string &model = std::string(), const Usage &usage = Usage());

    // 剥掉 <|im_end|> / <|endoftext|> 这类特殊 token(整串扫描, 不含文本内容)
    static std::string stripSpecialTokens(const std::string &text);

private:
    struct StreamState
    {
        enum Mode
        {
            Content,
            Think,
            Tool,
        };

        std::string buffer;      // 尚未消费的原始文本(可能是半个标签)
        Mode mode = Content;
        bool skipNewline = false;   // 进/出思考块后吞掉一个换行(可能晚一片才到)
        std::string closeTag;    // 当前工具调用的闭合标签
        std::string openTag;     // 当前工具调用的开标签(降级成正文时要拼回去)
        std::string toolName;    // <function=name> 形式里的 name
        std::string thinking;    // 累积的思考
        std::string content;     // 累积的正文(不含思考/工具调用)
        int callIndex = 0;       // 已解析出的工具调用数(用于生成 id)
        std::vector<Json::Value> calls;
    };

    // 消费 buffer, 把"确定是正文"的部分追加到 outText、"确定是思考"的部分追加到 outThinking
    void feed(const std::string &piece, bool final, std::string &outText, std::string &outThinking);

    bool parseToolCall(const std::string &openTag, const std::string &body,
                       const std::string &name, Json::Value &call);
    Json::Value makeChunk(const Json::Value &delta, const Json::Value &usage, const char *finish) const;

    JZAgentOmniConfig m_config;
    StreamState m_stream;
};

#endif // !JZ_AGENT_OMNI_H_
