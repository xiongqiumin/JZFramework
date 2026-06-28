#include "JZAgent.h"

#include <atomic>

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
        m_systemPrompt = system_root;
    else
        m_systemPrompt = "你是一个人助理,请友好的回答问题,并协助用户完成任务.";
    
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
        Json::Value history_msg = item.toJson();
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

        Json::Value func = tool.toJson();
        tool_def["function"] = func;

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
    req.setUrl(m_config.modelUrl);
    req.setHeader(JZNetworkRequest::ContentTypeHeader, "application/json");
    req.setRawHeader("Authorization", "Bearer " + m_config.modelToken);

    Json::FastWriter write;
    string json_str = write.write(request_body);

    JZByteArray req_buffer = str_toUtf8(json_str);
    m_llmReply = m_net->post(req, req_buffer);
    connect(&m_llmReply->sigFinished, this, &JZAgent::onLLMRequest);

    m_llmReply->setTimeOut(360 * 1000);
    m_llmReqInfo.requestTime = JZPlatform::getTickCountMs();
}

ToolDefine* JZAgent::getTool(std::string name)
{
    for (int i = 0; i < m_tools.size(); i++)
    {
        if (m_tools[i].name == name)
            return &m_tools[i];
    }

    return nullptr;
}

void JZAgent::processToolCall(ToolCall& tool)
{
    jzDebug() << "tool call:" << tool.function_name;

    ToolDefine *def = getTool(tool.function_name);
    if (!def)
    {
        ToolCallResult response;
        response.id = tool.id;
        response.isOk = false;
        response.result = "没有这个tool:" + tool.function_name;
        toolCallResult(response);
        return;
    }

    Json::Value param;
    Json::Reader reader;
    if (!reader.parse(tool.function_args, param))
    {
        ToolCallResult response;
        response.id = tool.id;
        response.isOk = false;
        response.result = "解析json失败: " + reader.getFormattedErrorMessages();
        toolCallResult(response);
        return;
    }

    tool.params = param;
    def->impl->call(this, tool);
}

// LLMHistory
Json::Value LLMHistory::toJson() const
{
    string role_str;
    Json::Value history_msg;

    if (role == Role::User)
        role_str = "user";
    else if (role == Role::Assistant)
        role_str = "assistant";
    else if (role == Role::System)
        role_str = "system";
    else if (role == Role::Tool)
        role_str = "tool";
    else
    {
        JZ_UNREACHABLE();
    }
    history_msg["role"] = role_str;
    history_msg["content"] = content;

    // 3. 处理 assistant 角色的 tool_calls 字段
    if (role == LLMHistory::Assistant && !toolRequest.empty())
    {
        Json::Value tool_calls_array(Json::arrayValue);
        for (const auto& tc : toolRequest)
        {
            tool_calls_array.append(tc.toJson());
        }
        history_msg["tool_calls"] = tool_calls_array;
    }

    // 4. 处理 tool 角色的 tool_call_id 字段
    if (role == LLMHistory::Tool && !toolResponseId.empty())
    {
        history_msg["tool_call_id"] = toolResponseId;
    }

    return history_msg;
}

bool LLMHistory::fromJson(const Json::Value& root)
{
    if (!root.isObject())
        return false;

    // 清空当前数据
    role = User;
    content.clear();
    toolRequest.clear();
    toolResponseId.clear();

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
    {
        return false;
    }

    // 2. 内容
    content = root["content"].asString();

    // 3. tool response id
    if (root.isMember("tool_response_id"))
        toolResponseId = root["tool_response_id"].asString();

    // 4. tool request 数组
    const Json::Value& tools = root["tool_request"];
    if (tools.isArray())
    {
        for (const auto& t : tools)
        {
            ToolCall call;
            // 这里依赖你 ToolCall 已有 fromJson
            // 如果没有，我下面也给你写好
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
        arr.append(h.toJson());
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
            LLMHistory h;
            h.fromJson(item);
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

JZAgentRequestPtr makeAgentParam(JZAgentRequest::Type param_type)
{
    static std::atomic<int> llm_param_id(0);
    JZAgentRequestPtr ptr = std::make_shared<JZAgentRequest>(param_type, llm_param_id++);
    return ptr;
}

void JZAgent::addTool(const ToolDefine& tool)
{
    m_tools.push_back(tool);
}

void JZAgent::toolCallResult(const ToolCallResult &response)
{
    JZAgentRequestPtr req = makeAgentParam(JZAgentRequest::ToolCallResultPrompt);
    req->toolCallResult = response;
    m_requests.push_back(req);
    llmLoop();
}

const ToolDefine* JZAgent::tool(const std::string& name)
{
    for (int i = 0; i < m_tools.size(); i++)
    {
        if (m_tools[i].name == name)
            return &m_tools[i];
    }
    return nullptr;
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

std::string JZAgent::getContent(const Json::Value& result)
{
    std::string content = result["choices"][0]["message"]["content"].asString();
    return content;
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

            LLMHistory chat;
            if (req->type == JZAgentRequest::UserPrompt)
            {
                jzDebug() << "user:" << req->userPrompt;

                chat.role = LLMHistory::User;
                chat.content = req->userPrompt;
            }
            else if (req->type == JZAgentRequest::SystemPrompt)
            {
                jzDebug() << "system:" << req->systemPrompt;

                chat.role = LLMHistory::System;
                chat.content = req->systemPrompt;
            }
            else if (req->type == JZAgentRequest::ToolCallResultPrompt)
            {
                jzDebug() << "tool result:" << req->toolCallResult.result;

                chat.role = LLMHistory::Tool;
                chat.content = req->toolCallResult.result;
                chat.toolResponseId = req->toolCallResult.id;

                if (!req->toolCallResult.isOk)
                    m_toolError++;
                else
                    m_toolError = 0;

                if (m_toolError >= 3)
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
        llmError(string("内部错误:") + e.what());
    }
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

    string json_message = str_fromUtf8(net_reply->readAll());

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

    Json::Value first_choice = result["choices"][0];
    Json::Value message = first_choice["message"];
    std::string content = message["content"].asString();
    std::string finish_reason = first_choice["finish_reason"].asString();

    Json::Value usage = result["usage"];
    if (usage.isObject())
    {
        int prompt_tokens = usage["prompt_tokens"].asInt();          // 输入
        int completion_tokens = usage["completion_tokens"].asInt();  // 输出
        int total_tokens = usage["total_tokens"].asInt();            // 总计
        
        jzDebug() << "延迟:" << llm_delay << "输入token:" << prompt_tokens 
            << "输出token:" << completion_tokens << "总token:" << total_tokens;

        m_context.usage.prompt_tokens += prompt_tokens;
        m_context.usage.completion_tokens += completion_tokens;
        m_context.usage.total_tokens += total_tokens;
    }

    LLMHistory reply;
    reply.role = LLMHistory::Assistant;
    reply.content = content;

    if (!content.empty())
    {
        jzDebug() << "assistant:" << content;
        sigReply.emit(content);
    }

    // 处理工具调用（tool_calls）场景 - OpenAI 标准格式
    if (message.isMember("tool_calls") && message["tool_calls"].isArray())
    {
        Json::Value tool_calls = message["tool_calls"];
        for (const auto& tc : tool_calls)
        {
            // 构建 tool_call 对象
            ToolCall tool_call;
            tool_call.id = tc["id"].asString();
            tool_call.type = tc["type"].asString();
            tool_call.function_name = tc["function"]["name"].asString();
            tool_call.function_args = tc["function"]["arguments"].asString();

            // 将工具调用添加到 history 的 toolRequest 中
            reply.toolRequest.push_back(tool_call);
            m_context.history.push_back(reply);

            processToolCall(tool_call);
        }
    }
    else
    {
        // 其他情况也保存到历史
        m_context.history.push_back(reply);
    }

    llmLoop();
}