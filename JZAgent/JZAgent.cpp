#include "JZAgent.h"

#include <atomic>
#include <sstream>

#include "JZApplication.h"
#include "JZDateTime.h"
#include "JZDebug.h"
#include "JZDir.h"
#include "JZElapsedTimer.h"
#include "JZTextStream.h"
#include "JZTimer.h"
#include "JZFile.h"
#include "JZStd.h"
#include "JZPlatform.h"

// JZAgent
JZAgent::JZAgent(JZObject* parent) : JZObject(parent), 
    sigReply(this), sigFinished(this), sigError(this)
{
    m_net = new JZNetworkAccessManager(this);
    m_llmReply = nullptr;
    m_state = None;

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

    m_replyHistory.clear();
    m_streamBuffer.clear();
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

void JZAgent::stop()
{
    llmEnd();
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
    m_currentRound++;
    if (m_currentRound >= m_config.maxRounds)
    {
        jzDebug() << "Max dialogue rounds reached:";
        llmError("执行任务超时");
        return;
    }

    // 每次请求重新累积（流式与非流式统一）
    m_replyHistory.clear();

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

    JZNetworkRequest req;
    req.setUrl(JZUrl(m_config.modelUrl));
    req.setHeader(JZNetworkRequest::ContentTypeHeader, "application/json");
    req.setRawHeader("Authorization", "Bearer " + m_config.modelToken);

    Json::FastWriter write;
    std::string json_str = write.write(request_body);

    JZByteArray req_buffer = str_toUtf8(json_str);
    m_llmReply = m_net->post(req, req_buffer);
    connect(&m_llmReply->sigFinished, this, &JZAgent::onLLMRequest);
    if (m_streamEnabled)
    {
        // 流式响应走分块(SSE)：实时读取增量
        connect(&m_llmReply->sigReadyRead, this, &JZAgent::onLLMStreamReadyRead);
        // 每次请求重新累积原始缓冲
        m_streamBuffer.clear();
    }

    m_llmReply->setTimeOut(30 * 1000);
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
    jzDebug() << "tool call:" << tool_call.function_name;

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
    history_msg["content"] = content;

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
    toolRequest.clear();
    toolResponseId.clear();

    if (!parseRole(root))
        return false;

    content = root["content"].asString();

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

void JZAgent::llmError(std::string reply)
{
    jzDebug() << "llm end:" << reply;
    sigError.emit(reply);
    llmEnd();
}

void JZAgent::llmEnd()
{
    clear();
    sigFinished.emit();
}

// 统一的响应增量处理
void JZAgent::processResponseDelta(const std::string &content,
                                   const Json::Value &tool_calls,
                                   const Json::Value &usage)
{
    // 单次回复对应唯一一条 assistant 历史，首次增量时创建
    if (m_replyHistory.empty())
    {
        auto reply = std::make_shared<LLMHistory>();
        reply->role = LLMHistory::Assistant;
        m_replyHistory.push_back(reply);
    }
    LLMHistory& reply = *m_replyHistory.back();

    if (!content.empty())
    {
        reply.content += content;
        sigReply.emit(content);   // 实时增量回调（流式与非流式一致）
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
        m_context.usage.prompt_tokens     += usage.get("prompt_tokens", 0).asInt();
        m_context.usage.completion_tokens += usage.get("completion_tokens", 0).asInt();
        m_context.usage.total_tokens      += usage.get("total_tokens", 0).asInt();
    }
}

void JZAgent::finalizeResponse()
{
    if (m_replyHistory.empty())
        return;

    auto reply = m_replyHistory.back();
    if (!reply->toolRequest.empty())
    {
        // assistant 消息只入历史一次，所有 tool_calls 复用同一条
        m_context.history.push_back(reply);
        for (auto& call : reply->toolRequest)
            processToolCall(call);
    }
    else
    {
        if (!reply->content.empty())
            m_context.history.push_back(reply);
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

    if (m_context.usage.total_tokens > 10 * 10000)
    {
        llmError("token out of limit: " + std::to_string(m_context.usage.total_tokens));
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
                jzDebug() << "user:" << req->userPrompt;

                chat->role = LLMHistory::User;
                chat->content = req->userPrompt;
            }
            else if (req->type == JZAgentRequest::SystemPrompt)
            {
                jzDebug() << "system:" << req->systemPrompt;

                chat->role = LLMHistory::System;
                chat->content = req->systemPrompt;
            }
            else if (req->type == JZAgentRequest::ToolCallResultPrompt)
            {
                jzDebug() << "tool result:" << req->toolCallResult.result;

                chat->role = LLMHistory::Tool;
                chat->content = req->toolCallResult.result;
                chat->toolResponseId = req->toolCallResult.id;

                if (!req->toolCallResult.isOk)
                    m_toolError++;
                else
                    m_toolError = 0;

                if (m_toolError >= m_config.maxToolError)
                {
                    llmError("当前服务不可用,请稍后重试");
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
        llmError(std::string("内部错误:") + e.what());
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

    m_streamBuffer.append(str_fromUtf8(data));

    // 按 SSE 事件边界 "\n\n" 切分，逐事件处理
    size_t pos = 0;
    while (true)
    {
        size_t sep = m_streamBuffer.find("\n\n", pos);
        if (sep == std::string::npos)
            break;
        std::string event = m_streamBuffer.substr(pos, sep - pos);
        pos = sep + 2;
        processSSEEvent(event);
    }
    // 保留不完整的尾部，等待后续数据
    m_streamBuffer.erase(0, pos);
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
        jzDebug() << "SSE payload 解析失败, 原始内容=[" << payload << "]";
        llmError("SSE数据解析错误: " + reader.getFormattedErrorMessages());
        return;
    }

    Json::Value choices = result["choices"];
    if (!choices.isArray() || choices.size() == 0)
        return;

    Json::Value delta = choices[0]["delta"];
    std::string content = delta["content"].asString();

    Json::Value tool_calls(Json::arrayValue);
    if (delta.isMember("tool_calls") && delta["tool_calls"].isArray())
        tool_calls = delta["tool_calls"];

    Json::Value usage = result["usage"];
    processResponseDelta(content, tool_calls, usage);
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
        llmError("网络错误" + net_reply->errorString());
        auto body = net_reply->readAll();
        if (!body.isEmpty())
        {
            jzWarning().noquote() << "网络错误:" << net_reply->errorString()
                << ", detail:" << str_fromUtf8(body);
        }
        else
        {
            jzWarning().noquote() << "网络错误:" + net_reply->errorString();
        }
        return;
    }

    // 流式模式：增量已通过 sigReadyRead -> processResponseDelta 累积进
    // m_replyHistory；仅在累积为空时（provider 忽略 stream 返回整包）降级按非流式整包解析。
    if (!m_streamEnabled || m_replyHistory.empty())
    {
        std::string json_message = str_fromUtf8(net_reply->readAll());

        Json::Value result;
        Json::Reader reader;
        if (!reader.parse(json_message, result))
        {
            llmError("json解析错误:" + reader.getFormattedErrorMessages());
            return;
        }

        Json::Value choices = result["choices"];
        if (!choices.isArray() || choices.size() == 0)
            throw std::runtime_error("LLM返回格式错误");

        Json::Value message = result["choices"][0]["message"];
        std::string content = message["content"].asString();

        Json::Value tool_calls(Json::arrayValue);
        if (message.isMember("tool_calls") && message["tool_calls"].isArray())
            tool_calls = message["tool_calls"];

        Json::Value usage = result["usage"];
        processResponseDelta(content, tool_calls, usage);
    }

    // 流式与非流式统一收尾
    finalizeResponse();
}
