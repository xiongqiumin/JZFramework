#include "JZAgent.h"

#include <atomic>
#include <algorithm>
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


class ReadSkillTool : public JZAgentTool
{
public:    
    // 统一调用接口：agent 在需要执行工具时传入自身与已解析的参数
    ReadSkillTool()
    {
        ParamSchema p;
        p.name = "name";
        p.type = STRING;
        p.description = "技能名称";

        ToolDefine def;
        def.name = "read_skill";
        def.description = "读取技能详情";
        def.parameters.push_back(p);
        def.required_parameters.push_back("name");
        setToolDefine(def);
    }

    virtual void call(const ToolCall &tool_call)
    {
        std::string name = tool_call.params.get("name", "").asString();
        
        auto skill_def = agent->skill(name);

        ToolCallResult result;
        result.id = tool_call.id;
        if (skill_def)
        {
            result.isOk = true;
            result.result = skill_def->usage;
        }
        else
        {
            result.isOk = false;
            result.result = "没有对应的skill";
        }
        
        sigResult.emit(result);
    }

    JZAgent *agent;
};


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
    sigReply(this), sigThinking(this), sigToolCall(this), sigToolResult(this)
    , sigFinished(this), sigError(this)
{
    m_net = new JZNetworkAccessManager(this);
    m_llmReply = nullptr;
    m_state = State::Inactive;
    m_readSkill = new ReadSkillTool();    
    m_readSkill->setParent(this);

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

    if (m_compressReply)
    {
        auto creply = m_compressReply;
        m_compressReply = nullptr;
        creply->close();
        creply->deleteLater();
    }
    m_compressChunk.clear();

    m_currentRound = -1;
    m_isActive = false;
    m_inTransaction = false;
    m_streamEnabled = false;
    m_thinkEnabled = true;
    m_toolError = 0;

    m_error.clear();
    m_errorCode = AgentError::None;  // 未设置错误时为 None

    m_state = State::Inactive;   // 重置：未运行/已停止
    m_replyHistory = std::shared_ptr<LLMHistory>();
}

// 一轮对话结束(Idle)后重开新任务: 仅 Idle 允许, 复位到 Inactive 并清空对话历史。
void JZAgent::reset()
{
    if (m_state != State::Idle)
    {
        const char *state = (m_state == State::Inactive)
            ? "Inactive(无需重置)" : "WaitingReply(请求在途, 请先 stop 或等 sigFinished)";
        jzWarning() << "JZAgent: reset() 被拒绝: 仅一轮对话结束后(Idle)可重置, 当前状态" << state;
        return;
    }

    clear();            // 复位运行态/错误/回复缓存, 内部置 Inactive
    m_context.clear();  // 清空对话历史与累计 usage
    m_requests.clear();
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

void JZAgent::setThinkEnabled(bool enabled)
{
    m_thinkEnabled = enabled;
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
                if (!ret.isOk())
                {
                    jzWarning() << "JZAgent: skip skill \"" << file_info.fileName()
                                << "\" : " << ret.errorMessage();
                    continue;
                }
                m_skills.push_back(ret.value());
            }
        }
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

bool JZAgent::rejectWhileActive(const char *what) const
{
    if (m_state != State::Inactive)
    {
        jzWarning() << "JZAgent: 修改" << what
                    << "被拒绝: agent 当前不处于 Inactive(已开始或结束过对话),"
                    << " 运行期改动会破坏当前会话的 KV 前缀缓存;"
                    << " 如需修改请先 init()/clear() 重置状态。";
        return true;
    }
    return false;
}

// 非 Inactive 时修改 system prompt 会改变请求前缀, 拒绝以保证前缀缓存稳定
void JZAgent::setSystemPrompt(std::string prompt)
{
    if (rejectWhileActive("system prompt"))
        return;

    m_systemPrompt = prompt;
}

std::string JZAgent::dumpToolsPrompt()
{
    std::string tools_propmpt;
    for (const auto& tool : m_tools)
    {
        tools_propmpt += tool->toolDefine().name + ": " + tool->toolDefine().description + "\n";
    }
    return tools_propmpt;
}

std::string JZAgent::dumpSystemPrompt()
{
    return m_systemPrompt + buildSkillSummary();
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

const std::vector<JZAgentRequestPtr> &JZAgent::requestList() const
{
    return m_requests;
}

void JZAgent::clearRequestList()
{
    m_requests.clear();
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

    // 去掉 YAML 值两侧的成对引号（"" 或 ''）
    auto stripQuotes = [](const std::string &s) -> std::string
    {
        if (s.size() >= 2 &&
            ((s.front() == '"' && s.back() == '"') ||
             (s.front() == '\'' && s.back() == '\'')))
        {
            return str_trimmed(s.substr(1, s.size() - 2));
        }
        return s;
    };

    // 3. 技能文件统一采用标准格式：YAML front matter 包裹的 Markdown
    //    （如 skill/*.skill.md）。首行与闭合行均为 "---"，
    //    name / description 取自头部，闭合 "---" 之后的正文整体作为 usage。
    int first_line_end = str_indexOf(context, "\n", 0);
    std::string first_line = str_trimmed(first_line_end == -1 ? context : context.substr(0, first_line_end));
    if (first_line != "---")
    {
        return JZResult<SkillDefine>::Error(
            "Skill format error: file must start with '---' front matter: " + skill_path);
    }

    // 4. 逐行扫描 front matter，收集 name / description 并定位闭合行
    std::string fm_name, fm_desc;
    int body_start = -1;
    int line_start = (first_line_end == -1) ? (int)context.size() : first_line_end + 1;
    while (line_start < (int)context.size())
    {
        int line_end = str_indexOf(context, "\n", line_start);
        if (line_end == -1)
            line_end = (int)context.size();

        std::string line = str_trimmed(context.substr(line_start, line_end - line_start));
        if (line == "---")
        {
            body_start = line_end + 1;  // 闭合行之后的正文起点
            break;
        }

        if (str_startsWith(line, "name:"))
            fm_name = str_trimmed(line.substr(5));
        else if (str_startsWith(line, "description:"))
            fm_desc = str_trimmed(line.substr(12));

        line_start = line_end + 1;
    }

    if (body_start == -1)
    {
        return JZResult<SkillDefine>::Error("Skill format error: unclosed '---' front matter: " + skill_path);
    }

    // 5. name 优先取 front matter（缺省用入参）；description -> tips；正文 -> usage
    if (!fm_name.empty())
        def.name = stripQuotes(fm_name);
    def.tips = stripQuotes(fm_desc);
    def.usage = str_trimmed(context.substr(body_start));

    if (def.tips.empty())
    {
        return JZResult<SkillDefine>::Error("Skill format error: missing or invalid 'description' in front matter");
    }
    if (def.usage.empty())
    {
        return JZResult<SkillDefine>::Error("Skill format error: missing or invalid skill body");
    }

    // 6. 返回成功结果
    return JZResult<SkillDefine>::Ok(std::move(def));
}

std::string JZAgent::buildSkillSummary()
{
    if (m_skills.size() == 0)
        return std::string();

    std::string summary = "\n## 技能列表\n";
    for (const auto& skill : m_skills)
    {
        // 拼接技能摘要（C++ string 拼接，无缓冲区溢出风险）
        summary += skill.name + ": " + skill.tips + "\n";
    }
    summary += "使用skill前先用read_skill读取技能详情";
    return summary;
}

Json::Value JZAgent::buildContext()
{
    Json::Value messages(Json::arrayValue);

    // 添加 system 角色的消息
    Json::Value system_message;
    system_message["role"] = "system";
    system_message["content"] = m_systemPrompt + buildSkillSummary();
    messages.append(system_message);

    for (const auto& item : m_context.history)
    {
        Json::Value history_msg = item->toJson();
        // auto_added 是内部标记（持久化/界面判断用），不发给大模型
        history_msg.removeMember("auto_added");
        messages.append(history_msg);
    }

    return messages;
}

// 构建大模型所需的工具JSON结构（逻辑不变，序列化函数已更新）
Json::Value JZAgent::buildTools()
{
    Json::Value tools(Json::arrayValue);
    if (m_skills.size() > 0)
    {
        Json::Value tool_def;
        tool_def["type"] = "function";
        tool_def["function"] = m_readSkill->toolDefine().toJson();
        tools.append(tool_def);
    }
    
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
    if (m_config.maxRounds != -1 && m_currentRound >= m_config.maxRounds)
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
    {
        request_body["stream"] = true;  // <-- 开启流式输出
        // 流式下默认不返回 usage，需显式要求，否则 token 统计恒为 0
        request_body["stream_options"]["include_usage"] = true;
    }

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

    if(!m_thinkEnabled)
    {
        Json::Value chat_template_kwargs(Json::objectValue);
        chat_template_kwargs["enable_thinking"] = m_thinkEnabled;

        Json::Value local_extra_body(Json::objectValue);
        request_body["chat_template_kwargs"] = chat_template_kwargs;
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
    sigToolCall.emit(tool_call);

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
    m_pendingTools[tool_call.id] = tool;
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

    history_msg["auto_added"] = autoAdded;
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

    autoAdded = root.get("auto_added", false).asBool();
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
    if (rejectWhileActive("tool"))
        return;

    JZ_ASSERT(tool->parent() == nullptr);
    tool->setParent(this);
    connect(&tool->sigResult, this, &JZAgent::onToolCallResult);
    m_tools.push_back(tool);
}

void JZAgent::addMcp(JZMcpClient *client)
{
    if (rejectWhileActive("mcp tool"))
        return;

    JZ_ASSERT(client->parent() == nullptr);
    // MCP 客户端是异步连接: 未就绪时 tools() 为空, 登记不到任何远端工具。
    // 使用方应在 sigReady 之后再 addMcp。
    if (!client->isReady())
    {
        jzWarning() << "JZMcpClient 尚未就绪(连接与工具列表未完成), 没有远端工具可登记:"
                    << " 请先 startXxx() 并等待 sigReady 后再 addMcp";
    }
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
    auto it = m_pendingTools.find(response.id);
    if(it != m_pendingTools.end())
    {
        m_pendingTools.erase(it);
    }

    JZAgentRequestPtr req = makeAgentParam(JZAgentRequest::ToolCallResultPrompt);
    req->toolCallResult = response;
    m_requests.push_back(req);
    sigToolResult.emit(response);
    JZTimer::singleShot(0,[this]{
        llmLoop();
    });
}

void JZAgent::addSkill(const SkillDefine& tool)
{
    if (rejectWhileActive("skill"))
        return;

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

        trimIncompleteHistory(m_context);
        llmEnd();
    }
}

void JZAgent::llmEnd()
{
    auto tool_it = m_pendingTools.begin();
    while(tool_it != m_pendingTools.end())
    {
        tool_it->second->cancel(tool_it->first);
        tool_it++;
    }
    m_pendingTools.clear();

    if (m_llmReply)
    {
        auto reply = m_llmReply;
        m_llmReply = nullptr;
        reply->close();
        reply->deleteLater();
    }

    if (m_compressReply)
    {
        auto creply = m_compressReply;
        m_compressReply = nullptr;
        creply->close();
        creply->deleteLater();
    }
    m_compressChunk.clear();

    m_currentRound = 0;
    m_isActive = false;
    m_inTransaction = false;
    m_toolError = 0;

    m_state = State::Idle;
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

    // 思考增量先于正式回复上抛：流式按到达顺序自然如此，
    // 非流式单包也借此保证 thinking 排在 content 之前（首个 sigReply 即代表思考结束）。
    if (!thinking.empty())
    {
        reply.thinking += thinking;
        sigThinking.emit(thinking);   // 思考过程实时增量上抛（与 sigReply 并行）
    }

    if (!content.empty())
    {
        reply.content += content;
        sigReply.emit(content);   // 首次 sigReply 即代表思考阶段结束，进入正式回复
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

        m_context.recalcUsage();  // reply 已并入历史，重算累计 usage
        if(m_requests.empty())
            llmEnd();
        else
            llmLoop();
    }
}

static std::string roleName(LLMHistory::Role r)
{
    switch (r)
    {
    case LLMHistory::User:       return "user";
    case LLMHistory::Assistant:  return "assistant";
    case LLMHistory::System:     return "system";
    case LLMHistory::Tool:       return "tool";
    }
    return "";
}

bool JZAgent::startCompressHistory()
{
    if (m_config.tokenStrategy != JZAgentConfig::TokenCompress)
        return false;
    if (m_compressReply)
        return false;   // 已有压缩请求在途，避免重入

    // 统计非 system 历史条目
    int nonSys = 0;
    for (auto& h : m_context.history)
        if (h->role != LLMHistory::System)
            nonSys++;
    if (nonSys <= 1)
        return false;   // 历史太短，没有可压缩的余量

    // 一次性把整段历史全部纳入压缩块（含其中的 system 消息），由其整体生成一条摘要，
    // 不做"只压最旧一半"的渐进式处理。最近一次用户输入尚在 m_requests 中、尚未入历史，
    // 因此不会被压缩掉，压缩结束后仍会按正常路径追加。
    m_compressChunk = m_context.history;
    if (m_compressChunk.empty())
        return false;

    // 序列化为可读对话文本交给大模型总结
    std::string conv;
    for (auto& h : m_compressChunk)
    {
        conv += "[" + roleName(h->role) + "]\n" + h->content + "\n\n";
    }

    // 构造压缩请求（非流式）：以"总结作为下一轮输入"的标准方式请求模型压缩
    Json::Value body(Json::objectValue);
    body["model"] = m_config.modelName;
    body["stream"] = false;

    Json::Value sysMsg(Json::objectValue);
    sysMsg["role"] = "system";
    sysMsg["content"] = "请协助用户总结之前的所有任务、完成情况，以及最近的请求，"
                        "整理成一段可作为下一轮输入的摘要。"
                        "不要编造信息，只输出摘要本身，不要输出任何解释或多余文字。";
    Json::Value userMsg(Json::objectValue);
    userMsg["role"] = "user";
    userMsg["content"] = conv + "\n\n开始总结";

    Json::Value msgs(Json::arrayValue);
    msgs.append(sysMsg);
    msgs.append(userMsg);
    body["messages"] = msgs;

    Json::FastWriter writer;
    JZNetworkRequest req;
    req.setUrl(JZUrl(m_config.modelUrl));
    req.setHeader(JZNetworkRequest::ContentTypeHeader, "application/json");
    req.setRawHeader("Authorization", "Bearer " + m_config.modelToken);

    m_compressReply = m_net->post(req, str_toUtf8(writer.write(body)));
    connect(&m_compressReply->sigFinished, this, &JZAgent::onCompressFinished);
    m_compressReply->setTimeOut(m_config.netTimeout);
    return true;
}

void JZAgent::onCompressFinished()
{
    auto reply = m_compressReply;
    m_compressReply = nullptr;
    if (!reply)
        return;
    reply->deleteLater();

    if (reply->error() != JZNetworkReply::NoError)
    {
        llmError(HttpError, "历史压缩请求失败: " + reply->errorString());
        m_compressChunk.clear();
        return;
    }

    std::string json_str = str_fromUtf8(reply->readAll());
    Json::Value result;
    Json::Reader reader;
    if (!reader.parse(json_str, result)
        || !result.isMember("choices")
        || !result["choices"].isArray()
        || result["choices"].size() == 0)
    {
        llmError(NoError, "历史压缩返回格式错误: " + json_str.substr(0, 500));
        m_compressChunk.clear();
        return;
    }
    std::string summary = result["choices"][0]["message"]["content"].asString();
    if (summary.empty())
    {
        llmError(NoError, "历史压缩返回为空");
        m_compressChunk.clear();
        return;
    }

    // 压缩块即整段历史，直接整体清空，再追加单条摘要。
    auto& hist = m_context.history;
    hist.clear();

    // 把整段返回作为下一轮的用户输入（user 消息）追加到历史末尾
    auto sum = std::make_shared<LLMHistory>();
    sum->role = LLMHistory::User;
    sum->content = summary;
    sum->autoAdded = true;   // 标记为系统自动添加，界面不显示、重建历史时跳过
    hist.push_back(sum);     // 摘要追加到历史末尾，其余消息位置保持不变
    m_compressChunk.clear();

    m_context.recalcUsage();  // 历史已压缩，重算累计 usage 使其回落
    llmLoop();                // 重新进入循环：若仍超预算会继续压缩，否则走正常请求流程
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
    // 压缩请求在途，等 onCompressFinished 重新驱动 llmLoop，期间不重入
    if (m_compressReply)
        return;

    if (m_context.usage.total_tokens > m_config.maxToken)
    {
        // 超出预算：按配置的策略处理历史
        if (m_config.tokenStrategy == JZAgentConfig::TokenCompress)
        {
            // 压缩：把最旧一段历史发给大模型做摘要，异步进行，期间阻塞重入
            if (!startCompressHistory())
            {
                llmError(MaxTokenError, "token out of limit (无法继续压缩): " + std::to_string(m_context.usage.total_tokens));
                return;
            }
            return;  // 等待 onCompressFinished 重新驱动 llmLoop
        }
        // TokenError 直接报错终止
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
        std::string body = payload;
        if (body.size() > 500)
            body = body.substr(0, 500) + "...(truncated)";
        llmError(NoError, "SSE数据解析错误: " + reader.getFormattedErrorMessages()
                            + " payload=[" + body + "]");
        return;
    }

    // provider 在流式下也可能直接返回错误体（如欠费 Arrearage），
    // 必须识别并终止，否则会当作空 SSE 静默空转挂死。
    if (result.isMember("error"))
    {
        Json::Value err = result["error"];
        std::string code = err.isMember("code") ? err["code"].asString() : "";
        std::string type = err.isMember("type") ? err["type"].asString() : "";
        std::string msg  = err.isMember("message") ? err["message"].asString() : "";
        if (msg.empty())
            msg = payload;
        std::string detail = msg;
        if (!code.empty() || !type.empty())
            detail += " (code=" + code + ", type=" + type + ")";
        llmError(HttpError, "LLM错误: " + detail);
        return;
    }

    // 流式收尾常带一个 choices=[] 但含 usage 的结尾包（需 stream_options.include_usage=true）。
    // 必须先提取 usage，避免被下面的空 choices 提前 return 吞掉，否则 token 统计恒为 0。
    Json::Value usage = result["usage"];

    Json::Value choices = result["choices"];
    if (!choices.isArray() || choices.size() == 0)
    {
        // 仅 usage 的结尾包：把本轮 token 用量并入累积（reply 已因前面的内容包存在）
        if (usage.isObject())
            processResponseDelta("", "", Json::Value(Json::arrayValue), usage);
        return;
    }

    Json::Value delta = choices[0]["delta"];
    std::string content = delta["content"].asString();
    std::string thinking = extractThinking(delta);

    Json::Value tool_calls(Json::arrayValue);
    if (delta.isMember("tool_calls") && delta["tool_calls"].isArray())
        tool_calls = delta["tool_calls"];

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
        // HTTP 层错误(4xx/5xx 等) 才有有效状态码，body 里常藏着真正原因（如阿里云欠费 Arrearage）；
        // 连接失败/超时/取消等“非 HTTP 错误”没有有效 HTTP 状态码，不应再打印 "(HTTP x)" 造成混淆。
        bool is_http_error = (net_reply->error() == JZNetworkReply::HttpError);
        int http_code = net_reply->httpStatusCode();
        std::string body = str_fromUtf8(net_reply->readAll());

        if (is_http_error)
        {
            // 优先解析 body 中的 error 结构，给出更详细的错误原因。
            Json::Value result;
            Json::Reader reader;
            if (!body.empty() && reader.parse(body, result) && result.isMember("error"))
            {
                Json::Value err = result["error"];
                std::string ecode = err.isMember("code")    ? err["code"].asString()    : "";
                std::string etype = err.isMember("type")    ? err["type"].asString()    : "";
                std::string emsg  = err.isMember("message") ? err["message"].asString() : "";
                if (emsg.empty())
                    emsg = body;
                std::string detail = emsg;
                if (!ecode.empty() || !etype.empty())
                    detail += " (code=" + ecode + ", type=" + etype + ")";
                llmError(errCode, "HTTP " + std::to_string(http_code) + " 错误: " + detail);
                return;
            }
            // 有状态码但 body 无法解析：退化用状态码 + 原文/错误字符串。
            std::string extra = body.empty() ? net_reply->errorString() : body;
            llmError(errCode, "HTTP " + std::to_string(http_code) + " 错误: " + extra);
        }
        else
        {
            // 非 HTTP 错误（连接失败 / 超时 / 用户取消等）：无有效 HTTP 状态码，只显示错误原因。
            llmError(errCode, net_reply->errorString());
        }
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
            // 把原始 body 片段一并带出，便于排查到底是空响应、HTML 错误页还是其他乱码
            std::string body = json_message;
            if (body.size() > 500)
                body = body.substr(0, 500) + "...(truncated)";
            llmError(NoError, "json解析错误:" + reader.getFormattedErrorMessages()
                                + " body=[" + body + "]");
            return;
        }

        // provider 可能返回错误体（如阿里云欠费 code=Arrearage）：HTTP 仍是 200，
        // 但 body 是 {"error":{...}} 没有 choices。优先识别错误体，
        // 把真实原因（欠费/限流/鉴权等）暴露出来，而不是笼统的"返回格式错误"。
        if (result.isMember("error"))
        {
            Json::Value err = result["error"];
            std::string code = err.isMember("code") ? err["code"].asString() : "";
            std::string type = err.isMember("type") ? err["type"].asString() : "";
            std::string msg  = err.isMember("message") ? err["message"].asString() : "";
            if (msg.empty())
                msg = json_message;
            std::string detail = msg;
            if (!code.empty() || !type.empty())
                detail += " (code=" + code + ", type=" + type + ")";
            llmError(HttpError, "LLM错误: " + detail);
            return;
        }

        Json::Value choices = result["choices"];
        if (!choices.isArray() || choices.size() == 0)
        {
            llmError(HttpError, "LLM返回格式错误(无 choices): " + json_message);
            return;
        }

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
