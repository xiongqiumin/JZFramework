#include "JZServerAgent.h"

#include <sstream>

#include "JZAgent.h"
#include "JZAgentCliServer.h"
#include "JZCodeTools.h"
#include "JZDateTime.h"
#include "JZFile.h"
#include "JZFileInfo.h"
#include "JZPlatform.h"
#include "JZTerminalTools.h"
#include "JZVirtualTerminal.h"

using std::string;

// ===========================================================================
// server 独有工具: 只有 server 进程知道的东西(集群视图 / 任务记录 / 子进程监管)
//   ⚠ 远端 cli 的工具表里没有这几个, 也不该有 —— 那是控制面权限, 不是执行面权限。
// ===========================================================================

// 集群视图: 在线 cli + 本机子进程 + 任务计数
class JZServerAgentsTool : public JZCodeTool
{
public:
    explicit JZServerAgentsTool(JZAgentCliServer *srv) : m_srv(srv)
    {
        ToolDefine def;
        def.name = "server_agents";
        def.description =
            "列出这个集群里的所有 agent: 远端 cli(在线/忙闲/节点/当前任务/工作区) 与 server 本机被监管的"
            "子进程, 以及任务总数/运行中/失败计数。给某台派任务前先用它确认 agentId。";
        setToolDefine(def);
    }

    void call(const ToolCall &params) override
    {
        finish(params, true, m_srv->snapshotAgents().toStyledString());
    }

private:
    JZAgentCliServer *m_srv;
};

// 给任意 agent(远端 cli 或 server 自己)派一条后台任务
class JZServerTaskSubmitTool : public JZCodeTool
{
public:
    explicit JZServerTaskSubmitTool(JZAgentCliServer *srv) : m_srv(srv)
    {
        ToolDefine def;
        def.name = "server_task_submit";
        def.description =
            "把一条任务派给指定 agent(agentId 来自 server_agents), 立刻返回 taskId, 不等待结果。"
            "之后用 server_task_query 查进度/结果。这是『让我以外的那台机器干活』的唯一入口。";
        def.parameters.push_back(makeParam("agent_id", ParamType::String, "目标 agent 的 id"));
        def.parameters.push_back(makeParam("prompt", ParamType::String, "任务描述(给那个 agent 的模型看)"));
        def.parameters.push_back(makeParam("verify_cmd", ParamType::String,
                                          "可选。验收命令(退出码 0 = 通过); 留空 = 跑完即通过"));
        def.required_parameters.push_back("agent_id");
        def.required_parameters.push_back("prompt");
        setToolDefine(def);
    }

    void call(const ToolCall &params) override
    {
        const string agentId = params.params.get("agent_id", "").asString();
        const string prompt = params.params.get("prompt", "").asString();
        if (agentId.empty() || prompt.empty())
        {
            finish(params, false, "agent_id 与 prompt 都是必填");
            return;
        }

        Json::Value body(Json::objectValue);
        body["agentId"] = agentId;
        body["prompt"] = prompt;
        const string verify = params.params.get("verify_cmd", "").asString();
        if (!verify.empty())
            body["verify_cmd"] = verify;
        body["wait"] = false;              // 工具里绝不能同步等: 会把 agent 自己卡死

        Json::Value ack;
        string err;
        const int code = m_srv->submitTaskForTool(body, ack, err);
        if (code >= 400)
        {
            finish(params, false, "下发失败(" + std::to_string(code) + "): " + err);
            return;
        }
        finish(params, true, ack.toStyledString());
    }

private:
    JZAgentCliServer *m_srv;
};

// 查任务记录(server 侧历史)
class JZServerTaskQueryTool : public JZCodeTool
{
public:
    explicit JZServerTaskQueryTool(JZAgentCliServer *srv) : m_srv(srv)
    {
        ToolDefine def;
        def.name = "server_task_query";
        def.description =
            "查任务记录。不给 task_id 时按时间倒序列出最近若干条(可按 agent_id / status 过滤);"
            "给 task_id 时返回那一条的详情。status: running/success/failed/cancelled/lost。";
        def.parameters.push_back(makeParam("task_id", ParamType::String, "任务 id(留空 = 列表)"));
        def.parameters.push_back(makeParam("agent_id", ParamType::String, "只看某个 agent(可选)"));
        def.parameters.push_back(makeParam("status", ParamType::String, "只看某个状态(可选)"));
        def.parameters.push_back(makeParam("limit", ParamType::Number, "最多返回多少条, 默认 20"));
        setToolDefine(def);
    }

    void call(const ToolCall &params) override
    {
        const string taskId = params.params.get("task_id", "").asString();
        if (!taskId.empty())
        {
            Json::Value one = m_srv->snapshotTask(taskId);
            if (one.isNull())
            {
                finish(params, false, "没有这个任务: " + taskId);
                return;
            }
            finish(params, true, one.toStyledString());
            return;
        }

        int limit = params.params.get("limit", 20).asInt();
        if (limit <= 0 || limit > 100)
            limit = 20;
        Json::Value list = m_srv->snapshotTasks(params.params.get("agent_id", "").asString(),
                                                params.params.get("status", "").asString(), limit);
        finish(params, true, list.toStyledString());
    }

private:
    JZAgentCliServer *m_srv;
};

// 启停 server 本机被监管的 agent_cli 子进程
class JZServerAgentControlTool : public JZCodeTool
{
public:
    explicit JZServerAgentControlTool(JZAgentCliServer *srv) : m_srv(srv)
    {
        ToolDefine def;
        def.name = "server_agent_control";
        def.description =
            "启停 server 本机被监管的 agent_cli 子进程(配置 agents[] 里的那些, 不是远端在线 cli):"
            "action = start / stop / restart。远端 cli 只能它自己连上来, 不能从这里拉起来。";
        def.parameters.push_back(makeParam("name", ParamType::String, "子进程名(配置 agents[].name)"));
        def.parameters.push_back(makeParam("action", ParamType::String, "start / stop / restart"));
        def.required_parameters.push_back("name");
        def.required_parameters.push_back("action");
        setToolDefine(def);
    }

    void call(const ToolCall &params) override
    {
        const string name = params.params.get("name", "").asString();
        const string action = params.params.get("action", "").asString();
        bool ok = false;
        if (name.empty())
        {
            finish(params, false, "缺少 name");
            return;
        }
        if (action == "start")
            ok = m_srv->spawnAgent(name);
        else if (action == "stop")
            ok = m_srv->stopAgent(name);
        else if (action == "restart")
            ok = m_srv->restartAgent(name);
        else
        {
            finish(params, false, "action 只能是 start / stop / restart");
            return;
        }
        finish(params, ok, ok ? (action + " 成功: " + name) : (action + " 失败: " + name + " (不存在或状态不符)"));
    }

private:
    JZAgentCliServer *m_srv;
};

// ===========================================================================
// 内置 agent 的工具宿主: 与 cli/node 一样, 用 ICodeToolHost 把区域配置送进通用工具。
// ===========================================================================
class JZServerAgentToolHost : public ICodeToolHost
{
public:
    explicit JZServerAgentToolHost(const JZServerAgentConfig &cfg)
    {
        m_config.zone.workspace = codeNormalizePath(cfg.rootDir);
        m_config.level = AccessLevel::Standard;
        m_config.allowPackageManager = false;
        m_config.allowNetwork = false;
    }

    const JZVirtualTerminalConfig &toolTerminalConfig() const override { return m_config; }

    bool toolReadFile(const std::string &absPath, std::string &text, std::string &error) override
    {
        JZFileInfo info(absPath);
        if (!info.exists())
        {
            error = "文件不存在: " + absPath;
            return false;
        }
        if (info.isDir())
        {
            error = "这是一个目录: " + absPath;
            return false;
        }
        if (info.size() > 8 * 1024 * 1024)
        {
            error = "文件过大(>8MB): " + absPath;
            return false;
        }

        JZFile file;
        if (!file.open(absPath, "rb"))
        {
            error = "打开文件失败(权限?): " + absPath;
            return false;
        }
        JZByteArray data = file.readAll();
        file.close();
        text.assign(data.data(), data.size());
        return true;
    }

private:
    JZVirtualTerminalConfig m_config;
};

// ===========================================================================
// JZServerAgent
// ===========================================================================
JZServerAgent::JZServerAgent(JZObject *parent)
    : JZObject(parent)
    , sigEvent(this)
{
}

JZServerAgent::~JZServerAgent()
{
}

string JZServerAgent::buildSystemPrompt() const
{
    std::ostringstream ss;
    ss << "你是 JZAgent 管理服务器(control plane)内置的运维 agent, 跑在 server 进程里。\n"
       << "你的工作区是 server 本机: " << m_cfg.rootDir << "\n\n"
       << "你能做三类事:\n"
       << "1. 看集群: server_agents 列出在线 cli(忙闲/节点/工作区)与本机子进程;\n"
       << "2. 派活: server_task_submit 把任务发给某台 agent(异步), server_task_query 查进度和结果;\n"
       << "3. 本机操作: terminal_run / terminal_cancel / read_file / get_datetime。\n\n"
       << "注意:\n"
       << "- 远端 agent 有它自己的工作区和权限, 你看不到它的文件, 只能通过任务把活交给它;\n"
       << "- 派任务后不要空转轮询: 用 server_task_query 有节奏地查, 或者先把 taskId 报给用户;\n"
       << "- 你跑在 server 上, 终端命令影响的是控制机, 不要在这里跑远端才能跑的东西。\n";
    if (!m_cfg.systemExtra.empty())
        ss << "\n" << m_cfg.systemExtra << "\n";
    return ss.str();
}

bool JZServerAgent::init(const JZServerAgentConfig &cfg, JZAgentCliServer *srv, string &error)
{
    m_cfg = cfg;
    m_srv = srv;

    if (m_cfg.rootDir.empty())
        m_cfg.rootDir = JZPlatform::applicationDirPath();
    m_cfg.rootDir = codeNormalizePath(m_cfg.rootDir);
    m_toolHost.reset(new JZServerAgentToolHost(m_cfg));
    if (m_cfg.modelUrl.empty())
    {
        error = "selfAgent.modelUrl 为空";
        return false;
    }

    m_agent = new JZAgent(this);
    m_agent->setStreamEnabled(true);
    m_agent->setThinkEnabled(true);

    JZAgentConfig ac;
    ac.rootPath = m_cfg.rootDir;
    ac.modelUrl = m_cfg.modelUrl;
    ac.modelName = m_cfg.modelName;
    ac.modelToken = m_cfg.modelToken;
    ac.netTimeout = m_cfg.netTimeout;
    ac.maxRounds = m_cfg.maxRounds;
    ac.maxToken = m_cfg.maxToken;
    ac.reuseConnection = false;
    ac.tokenStrategy = JZAgentConfig::TokenCompress;
    m_agent->init(ac);

    connect(&m_agent->sigReply, this, &JZServerAgent::onReply);
    connect(&m_agent->sigThinking, this, &JZServerAgent::onThinking);
    connect(&m_agent->sigToolCall, this, &JZServerAgent::onToolCall);
    connect(&m_agent->sigToolResult, this, &JZServerAgent::onToolResult);
    connect(&m_agent->sigError, this, &JZServerAgent::onError);
    connect(&m_agent->sigFinished, this, &JZServerAgent::onFinished);

    // 本机执行面
    m_terminal = new JZVirtualTerminal(this);
    m_terminal->setId("server");
    m_terminal->setTerminalConfig(m_toolHost->toolTerminalConfig());
    m_agent->addTool(new JZTerminalRunTool(m_terminal));
    m_agent->addTool(new JZTerminalCancelTool(m_terminal));
    auto *readFile = new JZReadFileTool();
    readFile->setHost(m_toolHost.get());
    m_agent->addTool(readFile);
    m_agent->addTool(new JZGetDateTimeTool());

    // server 独有: 集群视图 / 派活 / 查记录 / 子进程监管
    m_agent->addTool(new JZServerAgentsTool(srv));
    m_agent->addTool(new JZServerTaskSubmitTool(srv));
    m_agent->addTool(new JZServerTaskQueryTool(srv));
    m_agent->addTool(new JZServerAgentControlTool(srv));

    m_agent->setSystemPrompt(buildSystemPrompt());
    return true;
}

void JZServerAgent::submit(const string &taskId, const string &prompt, const string &verifyCmd)
{
    if (!m_agent)
        return;

    if (m_busy)
    {
        JZServerAgentEvent ev;
        ev.taskId = taskId;
        ev.type = "done";
        ev.data = "{\"status\":\"failed\",\"error\":\"server agent 正忙\"}";
        sigEvent.emit(ev);
        return;
    }

    m_taskId = taskId;
    m_busy = true;
    m_failed = false;
    m_cancelled = false;
    m_round = 0;
    m_reply.clear();
    m_error.clear();
    m_lastEvent.clear();

    // 上一轮结束后 agent 处于 Idle: 开新任务前 reset(保留 tools/system prompt)
    if (m_everRan)
        m_agent->reset();
    m_everRan = true;

    string text = prompt;
    if (!verifyCmd.empty())
        text += "\n\n(附注: 本 agent 在 server 进程内运行, 不参与提交方的 verify 门控;"
                " 验证命令仅作参考: " + verifyCmd + ")";

    emitEvent("start", Json::Value(prompt).toStyledString());
    m_agent->request(text);
}

void JZServerAgent::cancel()
{
    if (!m_agent || !m_busy)
        return;
    m_cancelled = true;
    if (m_terminal)
        m_terminal->cancel();   // stop() 只停止 Agent 等待, 在跑的那条命令要显式收尾
    m_agent->stop();
}

void JZServerAgent::emitEvent(const string &type, const string &data)
{
    m_lastEvent = type;
    JZServerAgentEvent ev;
    ev.taskId = m_taskId;
    ev.type = type;
    ev.data = data;
    sigEvent.emit(ev);
}

void JZServerAgent::onReply(const string &delta)
{
    if (!m_busy)
        return;
    m_reply += delta;
    emitEvent("reply", Json::Value(delta).toStyledString());
}

void JZServerAgent::onThinking(const string &delta)
{
    (void)delta;   // 思考不落记录(体积大, 也不进上下文)
}

void JZServerAgent::onToolCall(const ToolCall &call)
{
    if (!m_busy)
        return;
    ++m_round;
    Json::Value j(Json::objectValue);
    j["name"] = call.function_name;
    j["args"] = call.function_args;
    emitEvent("tool_call", j.toStyledString());
}

void JZServerAgent::onToolResult(const ToolCallResult &res)
{
    if (!m_busy)
        return;
    Json::Value j(Json::objectValue);
    j["ok"] = res.isOk;
    j["result"] = res.result.size() > 2000 ? res.result.substr(0, 2000) + "…" : res.result;
    emitEvent("tool_result", j.toStyledString());
}

void JZServerAgent::onError(const string &err)
{
    m_failed = true;
    m_error = err;
    emitEvent("error", Json::Value(err).toStyledString());
}

void JZServerAgent::onFinished()
{
    if (!m_busy)
        return;

    Json::Value r(Json::objectValue);
    r["taskId"] = m_taskId;
    r["agentId"] = m_cfg.agentId;
    r["status"] = m_cancelled ? "cancelled" : (m_failed ? "failed" : "success");
    r["agent_reply"] = m_reply;
    r["error"] = m_cancelled && m_error.empty() ? string("任务已取消") : m_error;
    r["verify_rounds"] = 0;      // server agent 不跑 verify 门控
    r["rounds"] = m_round;

    m_busy = false;
    emitEvent("done", r.toStyledString());
}
