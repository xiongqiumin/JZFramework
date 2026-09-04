#include "JZAgent.h"

#include <atomic>
#include <sstream>

#include "JZApplication.h"
#include "JZDateTime.h"
#include "JZMcpClient.h"
#include "JZDebug.h"
#include "JZDir.h"
#include "JZElapsedTimer.h"
#include "JZTextStream.h"
#include "JZTimer.h"
#include "JZFile.h"
#include "JZStd.h"
#include "JZPlatform.h"

// ==================== 多模态: 图片 data URL ====================
// 只解前 16 个 base64 字符(对应 12 字节)判断图片类型, 避免为了认类型把整图解一遍
static std::string detectImageMime(const std::string &base64)
{
    std::string head = base64.substr(0, 16);
    while (!head.empty() && (head.back() == '\n' || head.back() == '\r' || head.back() == ' '))
        head.pop_back();

    JZByteArray raw = JZByteArray::fromBase64(JZByteArray(head.data(), (int)head.size()));
    const char *d = raw.data();
    int n = raw.size();

    if (n >= 8 && (unsigned char)d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G')
        return "image/png";
    if (n >= 3 && (unsigned char)d[0] == 0xFF && (unsigned char)d[1] == 0xD8)
        return "image/jpeg";
    if (n >= 6 && d[0] == 'G' && d[1] == 'I' && d[2] == 'F')
        return "image/gif";
    if (n >= 12 && d[0] == 'R' && d[1] == 'I' && d[2] == 'F' && d[3] == 'F' &&
        d[8] == 'W' && d[9] == 'E' && d[10] == 'B' && d[11] == 'P')
        return "image/webp";
    if (n >= 2 && d[0] == 'B' && d[1] == 'M')
        return "image/bmp";

    return "image/png";   // 认不出来按 png 处理
}

// 各家推理模型的思考字段名不同: DeepSeek/Qwen=reasoning_content, Ollama=thinking, OpenAI=reasoning
// 取第一个非空的值, 没有则返回空串。
static std::string extractThinking(const Json::Value &node)
{
    const char *candidates[] = { "reasoning_content", "thinking", "reasoning" };
    for (const char *name : candidates)
    {
        if (node.isMember(name))
        {
            std::string v = node[name].asString();
            if (!v.empty())
                return v;
        }
    }
    return "";
}

// base64 文本 -> data URL; 本身就是 data URL / http 地址时原样返回
static std::string makeImageDataUrl(const JZByteArray &image_base64)
{
    std::string b64(image_base64.data(), image_base64.size());

    size_t b = b64.find_first_not_of(" \t\r\n");
    size_t e = b64.find_last_not_of(" \t\r\n");
    if (b == std::string::npos)
        return std::string();
    b64 = b64.substr(b, e - b + 1);

    if (b64.compare(0, 5, "data:") == 0 ||
        b64.compare(0, 7, "http://") == 0 ||
        b64.compare(0, 8, "https://") == 0)
        return b64;

    return "data:" + detectImageMime(b64) + ";base64," + b64;
}

// JZAgent
JZAgent::JZAgent(JZObject* parent) : JZObject(parent), 
    sigReply(this), sigFinished(this), sigError(this)
{
    m_net = new JZNetworkAccessManager(this);
    m_llmReply = nullptr;
    m_state = State::Inactive;

    clear();
}

JZAgent::~JZAgent()
{
}

void JZAgent::clear()
{
    if (m_llmReply)
    {
        auto reply = m_llmReply;
        m_llmReply = nullptr;
        reply->close();
        reply->deleteLater();
    }

    m_currentRound = -1;
    m_isActive = false;
    m_inTransaction = false;
    m_streamEnabled = false;
    m_toolError = 0;

    m_replyHistory = std::shared_ptr<LLMHistory>();

    m_state = State::Inactive;   // 重置：未运行/已停止
}

const LLMContext* JZAgent::context() const
{
    return &m_context;
}

bool JZAgent::streamEnabled() const
{
    return m_streamEnabled;
}

void JZAgent::setStreamEnabled(bool enabled)
{
    JZ_ASSERT(!m_isActive);
    m_streamEnabled = enabled;
}

std::string JZAgent::readFile(std::string filepath)
{
    JZFile file;
    
    if (!file.open(filepath, "rt"))
        return std::string();

    std::string ret = str_fromUtf8(file.readAll());
    return ret;
}

// 裁掉首尾不完整的对话段。
// 大模型的硬性要求: 一条 assistant 若带 tool_calls, 后面必须紧跟对应的 tool 返回消息。
// 中途停止/保存很容易停在"assistant 有 tool_calls 但没有 tool 返回"的状态,
// 直接恢复会把不一致的历史发出去而被拒绝, 所以这里裁到自洽为止。
static void trimIncompleteHistory(LLMContext &ctx)
{
    while (!ctx.history.empty())
    {
        const auto &last = ctx.history.back();
        // 末尾的 tool 返回、system 都不应作为结尾
        if (last->role == LLMHistory::Tool || last->role == LLMHistory::System)
        {
            ctx.history.pop_back();
            continue;
        }
        // 末尾 assistant 还挂着未执行的工具调用
        if (last->role == LLMHistory::Assistant && !last->toolRequest.empty())
        {
            ctx.history.pop_back();
            continue;
        }
        break;
    }

    // 开头若残留 tool 返回(其对应的 assistant 已被丢弃)也一并裁掉
    while (!ctx.history.empty() && ctx.history.front()->role == LLMHistory::Tool)
        ctx.history.erase(ctx.history.begin());
}

bool JZAgent::saveHistory(const std::string &filepath) const
{
    LLMContext ctx;
    ctx.history = m_context.history;
    trimIncompleteHistory(ctx);
    if (ctx.history.empty())
        return false;      // 没有可存的对话, 不留下空存档文件
    ctx.recalcUsage();

    Json::Value root = ctx.toJson();

    JZFile file;
    if (!file.open(filepath, "wb"))
        return false;

    std::string text = root.toStyledString();
    int written = file.write(str_toUtf8(text));
    file.close();
    return written == (int)text.size();
}

bool JZAgent::loadHistory(const std::string &filepath)
{
    std::string text = readFile(filepath);
    if (text.empty())
        return false;

    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(text, root))
        return false;
    if (!m_context.fromJson(root))
        return false;

    trimIncompleteHistory(m_context);
    m_context.recalcUsage();
    return true;
}

void JZAgent::init(JZAgentConfig config)
{
    m_config = config;

    std::string system_root = readFile(m_config.rootPath + "/system.md");
    if (!system_root.empty())
    {
        m_systemPrompt = system_root;
        if (m_systemPrompt.size() == 0 || m_systemPrompt.back() != '\n')
            m_systemPrompt.append("\n");

        JZDir skills_dir(m_config.rootPath + "/skills");
        if (skills_dir.exists())
        {
            // 使用JZDir遍历目录：仅文件、排除.和..、按名称排序
            std::vector<JZFileInfo> file_list =
                skills_dir.entryInfoList(JZDir::Dirs | JZDir::NoDotAndDotDot, JZDir::SortName);

            // 遍历所有技能文件
            for (const auto& file_info : file_list)
            {
                auto ret = loadSkill(file_info.fileName(), file_info.filePath() + "/skill.md");
                m_skills.push_back(ret.value());
            }
        }

        m_systemPrompt += "\n";
        m_systemPrompt += buildSkillSummary();
    }
    else
    {
        m_systemPrompt = "你是一个人助理,请友好的回答问题,并协助用户完成任务.";
    }
}

std::string JZAgent::systemPrompt()
{
    return m_systemPrompt;
}

void JZAgent::setSystemPrompt(std::string prompt)
{
    m_systemPrompt = prompt;
}

std::string JZAgent::toolsPrompt()
{
    Json::FastWriter writer;
    return writer.write(buildTools());
}

void JZAgent::transaction()
{
    m_inTransaction = true;
}

void JZAgent::commit()
{
    m_inTransaction = false;
    llmLoop();
}

void JZAgent::systemRequest(std::string sys_prompt)
{
    JZAgentRequestPtr req = makeAgentParam(JZAgentRequest::SystemPrompt);
    req->systemPrompt = sys_prompt;
    m_requests.push_back(req);
    llmLoop();
}

void JZAgent::request(std::string user_prompt)
{
    if (!m_isActive)
    {
        m_isActive = true;
        m_currentRound = 0;
        m_toolError = 0;
    }

    JZAgentRequestPtr req = makeAgentParam(JZAgentRequest::UserPrompt);
    req->userPrompt = user_prompt;
    m_requests.push_back(req);
    llmLoop();
}

void JZAgent::request(const std::string &text, const std::string &image_url)
{
    requestWithImages(text, { image_url });
}

void JZAgent::request(const std::string &text, const JZByteArray &image_base64)
{
    requestWithImages(text, { makeImageDataUrl(image_base64) });
}

void JZAgent::requestWithImages(const std::string &text, const std::vector<std::string> &images)
{
    if (!m_isActive)
    {
        m_isActive = true;
        m_currentRound = 0;
        m_toolError = 0;
    }

    JZAgentRequestPtr req = makeAgentParam(JZAgentRequest::UserPrompt);
    req->userPrompt = text;
    req->images = images;
    m_requests.push_back(req);
    llmLoop();
}

void JZAgent::stop()
{
    llmError(UserCancelError, "user cancel");
}

std::string JZAgent::errorString()
{
    return m_error;
}

JZResult<SkillDefine> JZAgent::loadSkill(std::string skill_name, const std::string& skill_path)
{
    // 1. 打开并读取文件（RAII 自动关闭，无需手动 close）
    JZFile file;
    if (!file.open(skill_path, "rt"))
    {
        return JZResult<SkillDefine>::Error("Failed to open skill file: " + skill_path);
    }

    JZTextStream stream(&file);
    std::string context = stream.readAll();
    // file 会自动关闭，无需手动调用

    // 2. 初始化 SkillDefine
    SkillDefine def;
    def.name = std::move(skill_name);

    // 3. 抽取公共解析逻辑（减少重复代码）
    auto extractSection = [&](std::string section_name, int start_pos) -> std::string
    {
        // 查找章节标题：# Description / # Usage
        int idx = str_indexOf(context, std::string(section_name), start_pos);
        if (idx == -1)
            return {};

        // 跳到下一行
        idx = str_indexOf(context, "\n", idx);
        if (idx == -1)
            return {};

        int content_start = idx + 1;
        int content_end = str_indexOf(context, "# ", content_start);
        if (content_end == -1)
        {
            content_end = (int)context.size();
        }

        if (content_start >= content_end)
            return {};
        return str_trimmed(context.substr(content_start, content_end - content_start));
    };

    // 4. 解析 Description
    std::string tips = extractSection("# Description", 0);
    if (tips.empty())
    {
        return JZResult<SkillDefine>::Error("Skill format error: missing or invalid # Description section");
    }
    def.tips = std::move(tips);

    // 5. 解析 Usage
    std::string usage = extractSection("# Usage", 0);
    if (usage.empty())
    {
        return JZResult<SkillDefine>::Error("Skill format error: missing or invalid # Usage section");
    }
    def.usage = std::move(usage);

    // 6. 返回成功结果
    return JZResult<SkillDefine>::Ok(std::move(def));
}

std::string JZAgent::buildSkillSummary()
{
    std::string summary = "## 自定义技能列表\n";
    if (m_skills.size() == 0)
    {
        summary += "当前无自定义技能\n";
    }
    else
    {
        for (const auto& skill : m_skills)
        {
            // 拼接技能摘要（C++ string 拼接，无缓冲区溢出风险）
            summary += skill.name + ": " + skill.tips + "\n";
        }   
    }
    return summary;
}

Json::Value JZAgent::buildContext()
{
    Json::Value messages(Json::arrayValue);

    // 添加 system 角色的消息
    Json::Value system_message;
    system_message["role"] = "system";
    system_message["content"] = m_systemPrompt;  // 引用系统提示词变量
    messages.append(system_message);

    for (const auto& item : m_context.history)
    {
        Json::Value history_msg = item->toJson();
        messages.append(history_msg);
    }

    return messages;
}

// 构建大模型所需的工具JSON结构（逻辑不变，序列化函数已更新）
Json::Value JZAgent::buildTools()
{
    Json::Value tools(Json::arrayValue);
    for (const auto& tool : m_tools)
    {
        Json::Value tool_def;
        tool_def["type"] = "function";
        tool_def["function"] = tool->toolDefine().toJson();
        tools.append(tool_def);
    }

    return tools;
}

void JZAgent::llmRequest()
{
    m_errorCode = AgentError::None;
    m_error.clear();
    m_currentRound++;
    if (m_currentRound >= m_config.maxRounds)
    {
        llmError(MaxRoundError, "执行任务超时");
        return;
    }

    // 每次请求重新累积（流式与非流式统一）
    m_replyHistory = std::shared_ptr<LLMHistory>();

    // 设置 model 字段
    Json::Value request_body;
    request_body["model"] = m_config.modelName;
    if (m_streamEnabled)
        request_body["stream"] = true;  // <-- 添加这一行，开启流式输出

    // 构建消息上下文
    Json::Value messages = buildContext();
    request_body["messages"] = messages;

    // 如果有注册工具，添加 tools 参数
    if (!m_tools.empty())
    {
        Json::Value tools = buildTools();
        request_body["tools"] = tools;
        request_body["tool_choice"] = "auto";
    }

    // 调用方自定义字段(各家私有参数)最后合并, 允许覆盖上面的默认值
    if (m_config.extraBody.isObject())
    {
        for (const std::string &key : m_config.extraBody.getMemberNames())
            request_body[key] = m_config.extraBody[key];
    }

    JZNetworkRequest req;
    req.setUrl(JZUrl(m_config.modelUrl));
    req.setHeader(JZNetworkRequest::ContentTypeHeader, "application/json");
    req.setRawHeader("Authorization", "Bearer " + m_config.modelToken);

    Json::FastWriter write;
    std::string json_str = write.write(request_body);

    JZByteArray req_buffer = str_toUtf8(json_str);
    m_llmReply = m_net->post(req, req_buffer);
    m_state = WaitingReply;   // 请求已发出：等待 LLM/工具响应（流式与非流式统一）
    connect(&m_llmReply->sigFinished, this, &JZAgent::onLLMRequest);
    if (m_streamEnabled)
    {
        // 流式响应走分块(SSE)：实时读取增量
        connect(&m_llmReply->sigReadyRead, this, &JZAgent::onLLMStreamReadyRead);
    }

    m_llmReply->setTimeOut(m_config.netTimeout);
    m_llmReqInfo.requestTime = JZPlatform::getTickCountMs();
}

JZAgentTool* JZAgent::getTool(const std::string &name)
{
    for (int i = 0; i < m_tools.size(); i++)
    {
        if (m_tools[i]->toolName() == name)
            return m_tools[i];
    }

    return nullptr;
}

void JZAgent::processToolCall(ToolCall& tool_call)
{
    JZAgentTool *tool = getTool(tool_call.function_name);
    if (!tool)
    {
        ToolCallResult response;
        response.id = tool_call.id;
        response.isOk = false;
        response.result = "没有这个tool:" + tool_call.function_name;
        onToolCallResult(response);
        return;
    }

    Json::Value param;
    Json::Reader reader;
    if (!reader.parse(tool_call.function_args, param))
    {
        ToolCallResult response;
        response.id = tool_call.id;
        response.isOk = false;
        response.result = "解析json失败: " + reader.getFormattedErrorMessages();
        onToolCallResult(response);
        return;
    }

    tool_call.params = param;
    // 统一接口：把已解析的参数交给工具，工具通过 sigResult 回传结果
    tool->call(tool_call);
}

// LLMHistory
std::string LLMHistory::roleToString(Role r)
{
    switch (r)
    {
    case User:       return "user";
    case Assistant:  return "assistant";
    case System:     return "system";
    case Tool:       return "tool";
    }
    JZ_UNREACHABLE();
    return "";
}

bool LLMHistory::parseRole(const Json::Value& root)
{
    std::string role_str = root["role"].asString();
    if (role_str == "user")
        role = User;
    else if (role_str == "assistant")
        role = Assistant;
    else if (role_str == "system")
        role = System;
    else if (role_str == "tool")
        role = Tool;
    else
        return false;
    return true;
}

Json::Value LLMHistory::toJson() const
{
    Json::Value history_msg;
    history_msg["role"] = roleToString(role);

    // 带图片时 content 必须是数组: [{type:text,...},{type:image_url,...}]
    // 不带图片时保持原来的字符串形式, 与单模态完全一致
    if (!images.empty())
    {
        Json::Value parts(Json::arrayValue);
        if (!content.empty())
        {
            Json::Value text_part(Json::objectValue);
            text_part["type"] = "text";
            text_part["text"] = content;
            parts.append(text_part);
        }
        for (const auto &url : images)
        {
            Json::Value image_part(Json::objectValue);
            image_part["type"] = "image_url";
            image_part["image_url"]["url"] = url;
            parts.append(image_part);
        }
        history_msg["content"] = parts;
    }
    else
    {
        history_msg["content"] = content;
    }

    // 处理 assistant 角色的 tool_calls 字段
    if (role == LLMHistory::Assistant && !toolRequest.empty())
    {
        Json::Value tool_calls_array(Json::arrayValue);
        for (const auto& tc : toolRequest)
            tool_calls_array.append(tc.toJson());
        history_msg["tool_calls"] = tool_calls_array;
    }

    // 处理 tool 角色的 tool_call_id 字段
    if (role == LLMHistory::Tool && !toolResponseId.empty())
        history_msg["tool_call_id"] = toolResponseId;

    return history_msg;
}

bool LLMHistory::fromJson(const Json::Value& root)
{
    if (!root.isObject())
        return false;

    content.clear();
    images.clear();
    toolRequest.clear();
    toolResponseId.clear();

    if (!parseRole(root))
        return false;

    // 单模态是字符串, 多模态是 content 数组
    const Json::Value &c = root["content"];
    if (c.isString())
    {
        content = c.asString();
    }
    else if (c.isArray())
    {
        for (const auto &part : c)
        {
            if (!part.isObject())
                continue;

            std::string type = part.get("type", "").asString();
            if (type == "text")
                content += part.get("text", "").asString();
            else if (type == "image_url")
                images.push_back(part["image_url"].get("url", "").asString());
        }
    }

    if (root.isMember("tool_response_id"))
        toolResponseId = root["tool_response_id"].asString();

    const Json::Value& tools = root["tool_request"];
    if (tools.isArray())
    {
        for (const auto& t : tools)
        {
            ToolCall call;
            call.fromJson(t);
            toolRequest.push_back(call);
        }
    }

    return true;
}

// LLMContext
LLMContext::LLMContext()
{
}

void LLMContext::clear()
{
    history.clear();
    usage = JZAgentTokenUsage();
}

void LLMContext::recalcUsage()
{
    usage = JZAgentTokenUsage();
    for (const auto& h : history)
    {
        usage.prompt_tokens     += h->usage.prompt_tokens;
        usage.completion_tokens += h->usage.completion_tokens;
        usage.total_tokens      += h->usage.total_tokens;
    }
}

Json::Value LLMContext::toJson() const
{
    Json::Value root;

    Json::Value arr(Json::arrayValue);
    for (const auto& h : history)
        arr.append(h->toJson());
    root["history"] = arr;

    Json::Value u;
    u["prompt_tokens"] = usage.prompt_tokens;
    u["completion_tokens"] = usage.completion_tokens;
    u["total_tokens"] = usage.total_tokens;
    root["usage"] = u;

    return root;
}

bool LLMContext::fromJson(const Json::Value& root)
{
    if (!root.isObject())
        return false;

    clear();

    // token
    auto& u = root["usage"];
    usage.prompt_tokens = u.get("prompt_tokens", 0).asInt();
    usage.completion_tokens = u.get("completion_tokens", 0).asInt();
    usage.total_tokens = u.get("total_tokens", 0).asInt();

    // history → 直接复用 fromJson
    auto& hist = root["history"];
    if (hist.isArray())
    {
        for (const auto& item : hist)
        {
            auto h = std::make_shared<LLMHistory>();
            h->fromJson(item);
            history.push_back(h);
        }
    }
    return true;
}

// JZAgentRequest
JZAgentRequest::JZAgentRequest(Type param_type, int param_id)
{
    type = param_type;
    id = param_id;
}

JZAgentRequestPtr JZAgent::makeAgentParam(JZAgentRequest::Type param_type)
{
    return std::make_shared<JZAgentRequest>(param_type, m_paramIdGen++);
}

void JZAgent::addTool(JZAgentTool *tool)
{
    JZ_ASSERT(tool->parent() == nullptr);
    tool->setParent(this);
    connect(&tool->sigResult, this, &JZAgent::onToolCallResult);
    m_tools.push_back(tool);
}

void JZAgent::addMcp(JZMcpClient *client)
{
    JZ_ASSERT(client->parent() == nullptr);
    // 客户端及其代理工具都归本 agent 所有
    client->setParent(this);
    connect(&client->sigDestroyed, this, &JZAgent::onMcpDestroyed);

    McpEntry entry;
    entry.client = client;
    for (auto *tool : client->tools())
    {
        if (getTool(tool->toolName()))
        {
            jzWarning() << "mcp tool name conflict, skipped:" << tool->toolName();
            continue;
        }

        // 工具归 client 所有, 这里只登记调用入口
        connect(&tool->sigResult, this, &JZAgent::onToolCallResult);
        m_tools.push_back(tool);
        entry.tools.push_back(tool);
    }
    m_mcpEntries.push_back(entry);
}

// client 被单独销毁时(其代理工具已随之析构), 把残留指针从工具表里摘掉。
// 注意: 此时不能再访问 client 的任何成员, 只能比对指针。
void JZAgent::onMcpDestroyed(JZObject *obj)
{
    for (auto it = m_mcpEntries.begin(); it != m_mcpEntries.end(); ++it)
    {
        if (it->client != obj)
            continue;

        for (auto *tool : it->tools)
        {
            for (auto ti = m_tools.begin(); ti != m_tools.end(); ++ti)
            {
                if (*ti == tool)
                {
                    m_tools.erase(ti);
                    break;
                }
            }
        }
        m_mcpEntries.erase(it);
        return;
    }
}

const JZAgentTool* JZAgent::tool(const std::string &name) const
{
    for (int i = 0; i < m_tools.size(); i++)
    {
        if (m_tools[i]->toolName() == name)
            return m_tools[i];
    }
    return nullptr;
}

void JZAgent::onToolCallResult(const ToolCallResult &response)
{
    JZAgentRequestPtr req = makeAgentParam(JZAgentRequest::ToolCallResultPrompt);
    req->toolCallResult = response;
    m_requests.push_back(req);
    JZTimer::singleShot(0,[this]{
        llmLoop();
    });
}

void JZAgent::addSkill(const SkillDefine& tool)
{
    m_skills.push_back(tool);
}

const SkillDefine* JZAgent::skill(const std::string& name)
{
    for (int i = 0; i < m_skills.size(); i++)
    {
        if (m_skills[i].name == name)
            return &m_skills[i];
    }
    return nullptr;
}

void JZAgent::llmError(AgentError error, std::string error_string)
{
    if(m_errorCode == AgentError::None)
    {
        m_errorCode = error;
        m_error = error_string;
        sigError.emit(error_string);
        llmEnd();
    }
}

void JZAgent::llmEnd()
{
    clear();
    sigFinished.emit();
}

// 统一的响应增量处理
void JZAgent::processResponseDelta(const std::string &content,
                                   const std::string &thinking,
                                   const Json::Value &tool_calls,
                                   const Json::Value &usage)
{
    // 单次回复对应唯一一条 assistant 历史，首次增量时创建
    if (!m_replyHistory)
    {
        m_replyHistory = std::make_shared<LLMHistory>();
        m_replyHistory->role = LLMHistory::Assistant;
    }
    LLMHistory& reply = *m_replyHistory;

    if (!content.empty())
    {
        reply.content += content;
        sigReply.emit(content);   // 实时增量回调（流式与非流式一致）
    }

    if (!thinking.empty())
    {
        reply.thinking += thinking;
        sigThinking.emit(thinking);   // 思考过程实时增量上抛（与 sigReply 并行）
    }

    // 累积 tool_calls：流式按 index 合并增量片段，非流式无 index 时直接追加
    if (tool_calls.isArray())
    {
        for (const auto& tc : tool_calls)
        {
            int index = tc.get("index", (int)reply.toolRequest.size()).asInt();
            if (index < 0)
                index = 0;
            if (index >= (int)reply.toolRequest.size())
                reply.toolRequest.resize(index + 1);

            ToolCall& call = reply.toolRequest[index];
            if (tc.isMember("id") && !tc["id"].asString().empty())
                call.id = tc["id"].asString();
            if (tc.isMember("type") && !tc["type"].asString().empty())
                call.type = tc["type"].asString();
            if (tc.isMember("function"))
            {
                const Json::Value& fn = tc["function"];
                if (fn.isMember("name") && !fn["name"].asString().empty())
                    call.function_name = fn["name"].asString();
                if (fn.isMember("arguments"))
                    call.function_args += fn["arguments"].asString();
            }
        }
    }

    if (usage.isObject())
    {
        int prompt = usage.get("prompt_tokens", 0).asInt();
        int completion = usage.get("completion_tokens", 0).asInt();
        int total = usage.get("total_tokens", 0).asInt();

        // 记录本轮（Assistant 回复）token 增量：reply 恒为 Assistant，usage 仅来自回复。
        // context.usage 不再在此累加，而是由 history 重算得到，避免裁剪历史后无法回落。
        reply.usage.prompt_tokens     = prompt;
        reply.usage.completion_tokens = completion;
        reply.usage.total_tokens      = total;
    }
}

void JZAgent::finalizeResponse()
{
    if (!m_replyHistory)
        return;

    auto reply = m_replyHistory;
    if (!reply->toolRequest.empty())
    {
        // assistant 消息只入历史一次，所有 tool_calls 复用同一条
        m_context.history.push_back(reply);
        m_context.recalcUsage();  // reply 已并入历史，重算累计 usage
        for (auto& call : reply->toolRequest)
            processToolCall(call);
    }
    else
    {
        if (!reply->content.empty())
            m_context.history.push_back(reply);

        m_state = Idle;   // 已产出最终回复且无需工具：本轮任务结束，进入空闲

        // 本轮任务结束：复位轮次/活跃标记，使下一次 request() 是一轮全新的任务。
        // 否则常驻型应用（如代码助手服务）里 m_currentRound 会跨任务一直累加，
        // 累计到 maxRounds 后直接报"执行任务超时"。
        m_currentRound = 0;
        m_isActive = false;
        m_toolError = 0;

        m_context.recalcUsage();  // reply 已并入历史，重算累计 usage
        if(m_requests.empty())
            sigFinished.emit();
        else
            llmLoop();
    }
}

void JZAgent::llmLoop()
{
    if (m_inTransaction)
        return;
    if (m_requests.empty())
        return;
    //正在请求 
    if (m_llmReply)
        return;

    if (m_context.usage.total_tokens > m_config.maxToken)
    {
        // 超出预算：按策略处理
        if (m_config.tokenStrategy == JZAgentConfig::TokenTruncate)
        {
            // 统计非 system 历史条目，砍掉最旧的一半（system 不砍）
            int nonSys = 0;
            for (const auto& h : m_context.history)
                if (h->role != LLMHistory::System)
                    nonSys++;
            int toRemove = nonSys / 2;
            if (toRemove > 0)
            {
                for (auto it = m_context.history.begin();
                     it != m_context.history.end() && toRemove > 0; )
                {
                    if ((*it)->role != LLMHistory::System)
                    {
                        it = m_context.history.erase(it);
                        toRemove--;
                    }
                    else
                        ++it;
                }
                m_context.recalcUsage();  // 历史已裁剪，重算累计 usage 使其回落
                llmRequest();
                return;
            }
        }
        // 无法继续裁剪（非 system 历史不足）则报错终止
        llmError(MaxTokenError, "token out of limit: " + std::to_string(m_context.usage.total_tokens));
        return;
    }

    try
    {
        std::vector<JZAgentRequestPtr> request_list = m_requests;
        m_requests.clear();

        // 处理响应
        for (int i = 0; i < request_list.size(); i++)
        {
            auto req = request_list[i];

            auto chat = std::make_shared<LLMHistory>();
            if (req->type == JZAgentRequest::UserPrompt)
            {
                chat->role = LLMHistory::User;
                chat->content = req->userPrompt;
                chat->images = req->images;
            }
            else if (req->type == JZAgentRequest::SystemPrompt)
            {
                chat->role = LLMHistory::System;
                chat->content = req->systemPrompt;
            }
            else if (req->type == JZAgentRequest::ToolCallResultPrompt)
            {
                chat->role = LLMHistory::Tool;
                chat->content = req->toolCallResult.result;
                chat->toolResponseId = req->toolCallResult.id;

                if (!req->toolCallResult.isOk)
                    m_toolError++;
                else
                    m_toolError = 0;

                if (m_toolError >= m_config.maxToolError)
                {
                    llmError(ToolCallError, "当前服务不可用,请稍后重试");
                    return;
                }
            }
            else if (req->type == JZAgentRequest::Stop)
            {
                llmEnd();
                return;
            }
            else
            {
                JZ_UNREACHABLE();
            }

            m_context.history.push_back(chat);
        }

        llmRequest();
    }
    catch (const std::exception& e)
    {
        llmError(NoError, std::string("内部错误:") + e.what());
    }
}

void JZAgent::onLLMStreamReadyRead()
{
    auto net_reply = (JZNetworkReply*)sender();
    if (net_reply != m_llmReply)
        return;

    JZByteArray data = net_reply->readAll();
    if (data.isEmpty())
        return;

    // readAll 已返回完整数据（可能包含一个或多个 SSE 事件），直接按事件边界切分处理
    std::string buffer = str_fromUtf8(data);
    size_t pos = 0;
    while (true)
    {
        size_t sep = buffer.find("\n\n", pos);
        if (sep == std::string::npos)
            break;
        std::string event = buffer.substr(pos, sep - pos);
        pos = sep + 2;
        processSSEEvent(event);
    }
}

void JZAgent::processSSEEvent(const std::string& event)
{
    std::istringstream iss(event);
    std::string line;
    while (std::getline(iss, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.compare(0, 5, "data:") != 0)
            continue;   // 跳过 :ping / event: 等非 data 行
        std::string payload = line.substr(5);
        size_t p = payload.find_first_not_of(' ');
        if (p != std::string::npos)
            payload = payload.substr(p);
        processSSEData(payload);
    }
}

void JZAgent::processSSEData(const std::string& payload)
{
    if (payload.empty())
        return;   // 空 data 行：SSE keep-alive 心跳，无内容，跳过
    if (payload == "[DONE]")
        return;

    Json::Value result;
    Json::Reader reader;
    if (!reader.parse(payload, result))
    {
        llmError(NoError, "SSE数据解析错误: " + reader.getFormattedErrorMessages());
        return;
    }

    Json::Value choices = result["choices"];
    if (!choices.isArray() || choices.size() == 0)
        return;

    Json::Value delta = choices[0]["delta"];
    std::string content = delta["content"].asString();
    std::string thinking = extractThinking(delta);

    Json::Value tool_calls(Json::arrayValue);
    if (delta.isMember("tool_calls") && delta["tool_calls"].isArray())
        tool_calls = delta["tool_calls"];

    Json::Value usage = result["usage"];
    processResponseDelta(content, thinking, tool_calls, usage);
}

void JZAgent::onLLMRequest()
{
    auto net_reply = (JZNetworkReply*)sender();
    net_reply->deleteLater();
    if (net_reply != m_llmReply)
        return;
    m_llmReply = nullptr;

    int64_t llm_delay = JZPlatform::getTickCountMs() - m_llmReqInfo.requestTime;
    m_llmReqInfo = LLMRequstInfo();

    if (net_reply->error() != JZNetworkReply::NoError)
    {
        AgentError errCode = NoError;
        switch (net_reply->error())
        {
            case JZNetworkReply::ConnectError:    errCode = ConnectError; break;
            case JZNetworkReply::HttpError:       errCode = HttpError; break;
            case JZNetworkReply::TimeOutError:    errCode = TimeOutError; break;
            case JZNetworkReply::UserCancelError: errCode = UserCancelError; break;
            default:                              errCode = ConnectError; break;
        }
        llmError(errCode, "网络错误" + net_reply->errorString());
        return;
    }

    // 流式模式：增量已通过 sigReadyRead -> processResponseDelta 累积进
    // m_replyHistory；仅在累积为空时（provider 忽略 stream 返回整包）降级按非流式整包解析。
    if (!m_streamEnabled || !m_replyHistory)
    {
        std::string json_message = str_fromUtf8(net_reply->readAll());

        Json::Value result;
        Json::Reader reader;
        if (!reader.parse(json_message, result))
        {
            llmError(NoError, "json解析错误:" + reader.getFormattedErrorMessages());
            return;
        }

        Json::Value choices = result["choices"];
        if (!choices.isArray() || choices.size() == 0)
            throw std::runtime_error("LLM返回格式错误");

        Json::Value message = result["choices"][0]["message"];
        std::string content = message["content"].asString();
        std::string thinking = extractThinking(message);

        Json::Value tool_calls(Json::arrayValue);
        if (message.isMember("tool_calls") && message["tool_calls"].isArray())
            tool_calls = message["tool_calls"];

        Json::Value usage = result["usage"];
        processResponseDelta(content, thinking, tool_calls, usage);
    }

    // 流式与非流式统一收尾
    finalizeResponse();
}
