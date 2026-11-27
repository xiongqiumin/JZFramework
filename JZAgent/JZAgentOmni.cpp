#include "JZAgentOmni.h"

#include <cstddef>

JZ_OBJECT_IMPL(JZAgentOmni, JZObject)

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

static bool startsWith(const std::string &text, const char *prefix)
{
    size_t len = 0;
    while (prefix[len] != '\0')
        len++;
    return text.size() >= len && text.compare(0, len, prefix) == 0;
}

// 紧凑输出, 且中文原样保留(默认 writer 会把非 ASCII 转成 \uXXXX, 模型读起来不自然)
static std::string jsonCompact(const Json::Value &value)
{
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    builder["commentStyle"] = "None";
    builder["emitUTF8"] = true;
    std::string text = Json::writeString(builder, value);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
        text.pop_back();
    return text;
}

// 模板里的 <think>\n 和 </think>\n\n 会带来多余换行, 进入/退出思考块时各吞掉一个
static void skipOneNewline(std::string &text)
{
    if (text.size() >= 2 && text[0] == '\r' && text[1] == '\n')
        text.erase(0, 2);
    else if (!text.empty() && text[0] == '\n')
        text.erase(0, 1);
}

// 取一条标准消息的文本部分; content 数组里的图片/音频换成占位标记。
static std::string contentText(const Json::Value &message, const JZAgentOmniConfig &config)
{
    const Json::Value &content = message["content"];
    if (content.isString())
        return content.asString();

    std::string text;
    if (content.isArray())
    {
        for (const Json::Value &part : content)
        {
            if (!part.isObject())
                continue;
            const std::string type = part.get("type", "").asString();
            if (type == "text")
                text += part.get("text", "").asString();
            else if (type == "image_url")
                text += config.imageMarker;
            else if (type == "input_audio" || type == "audio")
                text += config.audioMarker;
        }
    }
    return text;
}

// 历史里的 assistant(tool_calls) 还原成模型自己的工具调用标记
static std::string renderToolCall(const Json::Value &call, const JZAgentOmniConfig &config)
{
    const Json::Value &fn = call["function"];
    Json::Value body;
    body["name"] = fn.get("name", "").asString();

    const std::string arguments = fn.get("arguments", "").asString();
    Json::Value parsed;
    Json::Reader reader;
    if (!arguments.empty() && reader.parse(arguments, parsed))
        body["arguments"] = parsed;
    else if (!arguments.empty())
        body["arguments"] = arguments;

    return config.toolCallStart + jsonCompact(body) + config.toolCallEnd;
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

JZAgentOmni::JZAgentOmni(JZObject *parent)
    : JZObject(parent)
{
}

JZAgentOmni::~JZAgentOmni()
{
}

void JZAgentOmni::setConfig(const JZAgentOmniConfig &config)
{
    m_config = config;
}

const JZAgentOmniConfig &JZAgentOmni::config() const
{
    return m_config;
}

// ---------------------------------------------------------------------------
// 请求方向: 标准请求 -> 本地 prompt
// ---------------------------------------------------------------------------

std::string JZAgentOmni::buildPrompt(const Json::Value &request, int fromIndex) const
{
    bool thinking = m_config.enableThinking;
    const Json::Value &kwargs = request["chat_template_kwargs"];
    if (kwargs.isObject() && kwargs.isMember("enable_thinking"))
        thinking = kwargs["enable_thinking"].asBool();

    return buildPromptFromMessages(request["messages"], request["tools"], thinking, fromIndex);
}

std::string JZAgentOmni::buildPromptFromMessages(const Json::Value &messages, const Json::Value &tools,
                                                 bool enableThinking, int fromIndex) const
{
    if (!messages.isArray())
        return std::string();

    const int count = (int)messages.size();
    int start = fromIndex;
    if (start < 0)
        start = 0;
    if (start > count)
        start = count;

    const bool hasTools = m_config.inlineTools && tools.isArray() && tools.size() > 0;
    std::string prompt;

    // system 只在新会话(全量重放)时拼: KV 里已经有前文的增量模式不需要
    if (start == 0)
    {
        bool systemFound = false;
        for (int i = 0; i < count; i++)
        {
            if (messages[i].get("role", "").asString() != "system")
                continue;
            prompt += m_config.systemStart + contentText(messages[i], m_config);
            if (hasTools)
                prompt += m_config.toolsTitle + jsonCompact(tools);
            prompt += m_config.turnEnd;
            systemFound = true;
            break;
        }
        if (!systemFound && hasTools)
            prompt += m_config.systemStart + m_config.toolsTitle + jsonCompact(tools) + m_config.turnEnd;
    }

    for (int i = start; i < count; i++)
    {
        const Json::Value &message = messages[i];
        const std::string role = message.get("role", "").asString();

        // system 已单独处理; thinking / error 是本框架的存档消息, 从不发给模型
        if (role == "system" || role == "thinking" || role == "error")
            continue;

        if (role == "assistant")
        {
            prompt += m_config.assistantStart;

            std::string thinking = message.get("reasoning_content", "").asString();
            if (thinking.empty())
                thinking = message.get("thinking", "").asString();
            prompt += m_config.thinkStart;
            if (!thinking.empty())
                prompt += thinking + "\n";
            prompt += m_config.thinkEnd;

            prompt += contentText(message, m_config);

            const Json::Value &calls = message["tool_calls"];
            if (calls.isArray())
            {
                for (const Json::Value &call : calls)
                    prompt += renderToolCall(call, m_config);
            }
            prompt += m_config.turnEnd;
        }
        else if (role == "tool")
        {
            // 本地模板没有 tool 角色: 用 user 轮包一段 <tool_response>
            prompt += m_config.userStart + m_config.toolResponseStart;
            prompt += contentText(message, m_config);
            prompt += m_config.toolResponseEnd + m_config.turnEnd;
        }
        else
        {
            // user(以及未知角色)
            prompt += m_config.userStart + contentText(message, m_config) + m_config.turnEnd;
        }
    }

    // 补 assistant 头开始生成: 关闭思考时预填一个空的思考块
    std::string lastRole;
    for (int i = count - 1; i >= 0; i--)
    {
        lastRole = messages[i].get("role", "").asString();
        if (lastRole != "thinking" && lastRole != "error")
            break;
    }
    if (lastRole != "assistant")
    {
        prompt += m_config.assistantStart + m_config.thinkStart;
        if (!enableThinking)
            prompt += m_config.thinkEnd;
    }

    return prompt;
}

// ---------------------------------------------------------------------------
// 响应方向
// ---------------------------------------------------------------------------

std::string JZAgentOmni::stripSpecialTokens(const std::string &text)
{
    std::string out;
    out.reserve(text.size());

    size_t i = 0;
    while (i < text.size())
    {
        if (text[i] == '<' && i + 1 < text.size() && text[i + 1] == '|')
        {
            size_t end = text.find("|>", i + 2);
            if (end != std::string::npos)
            {
                i = end + 2;   // 整段 <|...|> 丢掉
                continue;
            }
        }
        out.push_back(text[i]);
        i++;
    }
    return out;
}

void JZAgentOmni::beginStream()
{
    m_stream = StreamState();
}

void JZAgentOmni::feed(const std::string &piece, bool final, std::string &outText, std::string &outThinking)
{
    m_stream.buffer += piece;

    while (true)
    {
        // 跨分片也安全: 换行可能晚一片才到, 用标记延迟吞
        if (m_stream.skipNewline && !m_stream.buffer.empty())
        {
            m_stream.skipNewline = false;
            skipOneNewline(m_stream.buffer);
        }

        if (m_stream.mode == StreamState::Content)
        {
            size_t lt = m_stream.buffer.find('<');
            if (lt == std::string::npos)
            {
                outText += m_stream.buffer;
                m_stream.content += m_stream.buffer;
                m_stream.buffer.clear();
                return;
            }
            if (lt > 0)
            {
                std::string text = m_stream.buffer.substr(0, lt);
                outText += text;
                m_stream.content += text;
                m_stream.buffer.erase(0, lt);
            }

            // <|im_end|> 这类特殊 token: 整段丢掉
            if (m_stream.buffer.size() >= 2 && m_stream.buffer[1] == '|')
            {
                size_t end = m_stream.buffer.find("|>", 2);
                if (end != std::string::npos)
                {
                    m_stream.buffer.erase(0, end + 2);
                    continue;
                }
                if (!final)
                    return;   // 等更多分片
                outText += m_stream.buffer;
                m_stream.content += m_stream.buffer;
                m_stream.buffer.clear();
                return;
            }

            // 以 '<' 开头: 判断是思考 / 工具调用 / 普通小于号
            size_t gt = m_stream.buffer.find('>');
            if (gt == std::string::npos)
            {
                if (!final)
                    return;   // 等更多分片
                outText += m_stream.buffer;
                m_stream.content += m_stream.buffer;
                m_stream.buffer.clear();
                return;
            }

            const std::string tag = m_stream.buffer.substr(0, gt + 1);
            if (tag == "<think>" || tag == "<thinking>")
            {
                m_stream.buffer.erase(0, gt + 1);
                m_stream.skipNewline = true;
                m_stream.mode = StreamState::Think;
                continue;
            }
            if (tag == m_config.toolCallStart)
            {
                m_stream.openTag = tag;
                m_stream.closeTag = m_config.toolCallEnd;
                m_stream.toolName.clear();
                m_stream.buffer.erase(0, gt + 1);
                m_stream.mode = StreamState::Tool;
                continue;
            }
            if (startsWith(tag, "<function=") && tag.size() > 11)
            {
                m_stream.openTag = tag;
                m_stream.closeTag = "</function>";
                m_stream.toolName = tag.substr(10, tag.size() - 11);
                m_stream.buffer.erase(0, gt + 1);
                m_stream.mode = StreamState::Tool;
                continue;
            }

            // 普通 '<'(如 "a < b"): 原样输出
            outText += '<';
            m_stream.content += '<';
            m_stream.buffer.erase(0, 1);
            continue;
        }

        if (m_stream.mode == StreamState::Think)
        {
            static const char *kCloseThink = "</think>";
            static const char *kCloseThinking = "</thinking>";

            size_t pos = m_stream.buffer.find(kCloseThink);
            size_t tagLen = 8;
            size_t other = m_stream.buffer.find(kCloseThinking);
            if (pos == std::string::npos || (other != std::string::npos && other < pos))
            {
                pos = other;
                tagLen = 11;
            }

            if (pos == std::string::npos)
            {
                if (!final)
                    return;
                outThinking += m_stream.buffer;
                m_stream.thinking += m_stream.buffer;
                m_stream.buffer.clear();
                m_stream.mode = StreamState::Content;
                return;
            }

            std::string thinking = m_stream.buffer.substr(0, pos);
            outThinking += thinking;
            m_stream.thinking += thinking;
            m_stream.buffer.erase(0, pos + tagLen);
            m_stream.skipNewline = true;
            m_stream.mode = StreamState::Content;
            continue;
        }

        // StreamState::Tool
        {
            size_t pos = m_stream.buffer.find(m_stream.closeTag);
            if (pos == std::string::npos)
            {
                if (!final)
                    return;
                // 输出被截断(只有开标签): 降级成正文, 不丢内容
                std::string raw = m_stream.openTag + m_stream.buffer;
                outText += raw;
                m_stream.content += raw;
                m_stream.buffer.clear();
                m_stream.mode = StreamState::Content;
                return;
            }

            std::string body = m_stream.buffer.substr(0, pos);
            m_stream.buffer.erase(0, pos + m_stream.closeTag.size());

            Json::Value call;
            if (parseToolCall(m_stream.openTag, body, m_stream.toolName, call))
            {
                m_stream.calls.push_back(call);
            }
            else
            {
                // JSON 不合法 / 缺 name: 当正文, 保证信息不丢
                std::string raw = m_stream.openTag + body + m_stream.closeTag;
                outText += raw;
                m_stream.content += raw;
            }
            m_stream.mode = StreamState::Content;
            continue;
        }
    }
}

bool JZAgentOmni::parseToolCall(const std::string &openTag, const std::string &body,
                                const std::string &name, Json::Value &call)
{
    Json::Value fn;

    if (!name.empty())
    {
        // <function=NAME><parameter=k>v</parameter>...</function>
        fn["name"] = name;
        Json::Value args(Json::objectValue);

        size_t pos = 0;
        while (true)
        {
            size_t begin = body.find("<parameter=", pos);
            if (begin == std::string::npos)
                break;
            size_t gt = body.find('>', begin);
            if (gt == std::string::npos)
                break;
            std::string key = body.substr(begin + 11, gt - begin - 11);
            size_t end = body.find("</parameter>", gt);
            if (end == std::string::npos)
                break;
            args[key] = body.substr(gt + 1, end - gt - 1);
            pos = end + 12;
        }

        fn["arguments"] = jsonCompact(args);
    }
    else
    {
        // <tool_call>{"name":..,"arguments":..}</tool_call>
        Json::Value root;
        Json::Reader reader;
        if (!reader.parse(body, root))
            return false;
        if (root.isMember("function"))
            root = root["function"];
        if (!root.isObject())
            return false;

        const std::string toolName = root.get("name", "").asString();
        if (toolName.empty())
            return false;
        fn["name"] = toolName;

        const Json::Value &arguments = root["arguments"];
        if (arguments.isString())
            fn["arguments"] = arguments.asString();
        else if (arguments.isObject() || arguments.isArray())
            fn["arguments"] = jsonCompact(arguments);
        else
            fn["arguments"] = "{}";
    }

    call["id"] = "call_" + std::to_string(m_stream.callIndex++);
    call["type"] = "function";
    call["function"] = fn;
    return true;
}

Json::Value JZAgentOmni::makeChunk(const Json::Value &delta, const Json::Value &usage, const char *finish) const
{
    Json::Value chunk;
    chunk["object"] = "chat.completion.chunk";
    chunk["created"] = 0;

    Json::Value choice;
    choice["index"] = 0;
    choice["delta"] = delta;
    if (finish)
        choice["finish_reason"] = finish;
    else
        choice["finish_reason"] = Json::Value(Json::nullValue);
    chunk["choices"].append(choice);

    if (!usage.isNull())
        chunk["usage"] = usage;

    return chunk;
}

Json::Value JZAgentOmni::parseDelta(const std::string &piece)
{
    std::string text;
    std::string thinking;
    feed(piece, false, text, thinking);

    Json::Value delta(Json::objectValue);
    if (!thinking.empty())
        delta["reasoning_content"] = thinking;
    if (!text.empty())
        delta["content"] = text;

    return makeChunk(delta, Json::Value(Json::nullValue), nullptr);
}

Json::Value JZAgentOmni::endStream(const std::string &model, const Usage &usage)
{
    std::string text;
    std::string thinking;
    feed(std::string(), true, text, thinking);

    Json::Value delta(Json::objectValue);
    if (!thinking.empty())
        delta["reasoning_content"] = thinking;
    if (!text.empty())
        delta["content"] = text;

    if (!m_stream.calls.empty())
    {
        Json::Value calls(Json::arrayValue);
        for (const Json::Value &call : m_stream.calls)
            calls.append(call);
        delta["tool_calls"] = calls;
    }

    Json::Value chunkUsage(Json::objectValue);
    chunkUsage["prompt_tokens"] = usage.prompt_tokens;
    chunkUsage["completion_tokens"] = usage.completion_tokens;
    chunkUsage["total_tokens"] = usage.prompt_tokens + usage.completion_tokens;

    const char *finish = m_stream.calls.empty() ? "stop" : "tool_calls";
    Json::Value chunk = makeChunk(delta, chunkUsage, finish);
    if (!model.empty())
        chunk["model"] = model;
    return chunk;
}

Json::Value JZAgentOmni::parseResponse(const std::string &raw, const std::string &model, const Usage &usage)
{
    beginStream();

    std::string text;
    std::string thinking;
    feed(raw, true, text, thinking);

    Json::Value message;
    message["role"] = "assistant";
    message["content"] = text;
    if (!thinking.empty())
        message["reasoning_content"] = thinking;
    if (!m_stream.calls.empty())
    {
        Json::Value calls(Json::arrayValue);
        for (const Json::Value &call : m_stream.calls)
            calls.append(call);
        message["tool_calls"] = calls;
    }

    Json::Value choice;
    choice["index"] = 0;
    choice["message"] = message;
    choice["finish_reason"] = m_stream.calls.empty() ? "stop" : "tool_calls";

    Json::Value response;
    response["id"] = "chatcmpl-local";
    response["object"] = "chat.completion";
    response["created"] = 0;
    if (!model.empty())
        response["model"] = model;
    response["choices"].append(choice);

    Json::Value responseUsage(Json::objectValue);
    responseUsage["prompt_tokens"] = usage.prompt_tokens;
    responseUsage["completion_tokens"] = usage.completion_tokens;
    responseUsage["total_tokens"] = usage.prompt_tokens + usage.completion_tokens;
    response["usage"] = responseUsage;

    return response;
}
