#include "JZTerminalTools.h"

using namespace std;

namespace
{
    string cwdOf(JZVirtualTerminal *term)
    {
        string cwd;
        if (term && term->printDir(cwd) && !cwd.empty())
            return cwd;
        return string();
    }

    ToolDefine makeDef(const string &name, const string &desc,
                      const vector<ParamSchema> &params, const vector<string> &required)
    {
        ToolDefine def;
        def.name = name;
        def.description = desc;
        def.parameters = params;
        def.required_parameters = required;
        return def;
    }
}

// ======================================================================
// JZTerminalToolBase
// ======================================================================
JZTerminalToolBase::JZTerminalToolBase(const std::string &name, JZVirtualTerminal *term)
    : m_term(term)
{
    setToolDefine(makeDef(name, "[deprecated] 本地终端命令工具。新链路请走 JZAgentNode 的 terminal_* 工具。",
                          vector<ParamSchema>(), vector<string>()));
}

// ======================================================================
// JZTerminalRunTool
// ======================================================================
JZTerminalRunTool::JZTerminalRunTool(JZVirtualTerminal *term)
    : JZTerminalToolBase("terminal_run", term)
{
    vector<ParamSchema> params;
    params.push_back(JZCodeTool::makeParam("command", ParamType::String, "要执行的命令"));
    params.push_back(JZCodeTool::makeParam("timeout_ms", ParamType::Number, "超时毫秒(默认 120000)"));
    params.push_back(JZCodeTool::makeParam("error_context", ParamType::Boolean,
                                           "输出过滤开关(默认 true, 所有命令都截断, 编译命令特殊处理): "
                                           "非编译超长保留头50+尾50行; 编译成功取尾部20行、失败取开头约100行错误"
                                           "(默认 true; false=原样全量)"));
    setToolDefine(makeDef("terminal_run",
                          "在沙箱终端执行一条 shell 命令(本地 code_agent 单用户终端)。"
                          "可以同时跑多条: 一条长任务不会堵住别的命令(每条各自一个 run_id)。",
                          params, { "command" }));
}

void JZTerminalRunTool::call(const ToolCall &params)
{
    if (!m_term)
    {
        finish(params, false, "终端未初始化");
        return;
    }

    Json::Value p = params.params;
    string command = p.get("command", "").asString();
    if (command.empty())
    {
        finish(params, false, "command 不能为空");
        return;
    }
    int timeoutMs = p.get("timeout_ms", 120000).asInt();
    if (timeoutMs < 1000)
        timeoutMs = 120000;

    // 输出过滤开关(默认开): 按次给(终端能同时跑多条, 用终端级开关会互相串)
    JZVirtualTerminal::OutputFilter filter = p.get("error_context", true).asBool()
                                                 ? JZVirtualTerminal::OutputFilter::On
                                                 : JZVirtualTerminal::OutputFilter::Off;

    // 命令被拒时 run() 会**同步**回调一次(ok=false), 用它记一下, 免得下面再回一次结果。
    auto fired = std::make_shared<bool>(false);
    JZResult<string> started = m_term->run(command, timeoutMs,
                                           [this, params, fired](bool ok, const string &out, const string &err)
                                           {
                                               *fired = true;
                                               m_runs.erase(params.id);
                                               string text = out;
                                               if (!err.empty())
                                               {
                                                   if (!text.empty() && text.back() != '\n')
                                                       text += "\n";
                                                   text += "[错误] " + err;
                                               }
                                               if (text.empty())
                                                   text = ok ? "(无输出)" : err;
                                               string cwd = cwdOf(m_term);
                                               if (!cwd.empty())
                                                   text += "\n[cwd] " + cwd;
                                               finish(params, ok, text);
                                           },
                                           nullptr, false, filter);
    if (started.isError())
    {
        if (!*fired)
            finish(params, false, started.errorMessage());
        return;
    }
    // 记下 run_id: 取消时按它精确停这一条(见 cancel)
    m_runs[params.id] = started.value();
}

void JZTerminalRunTool::cancel(const std::string &id)
{
    if (!m_term)
        return;
    // 只停这条调用对应的运行(同一终端里别的在跑命令不动)
    auto it = m_runs.find(id);
    if (it == m_runs.end())
        return;
    string runId = it->second;
    m_runs.erase(it);
    m_term->cancel(runId);
}

// ======================================================================
// JZTerminalOpenTool / JZTerminalListTool / JZTerminalCloseTool
// 单用户终端: 这些动作退化为"查看/常驻"语义
// ======================================================================
JZTerminalOpenTool::JZTerminalOpenTool(JZVirtualTerminal *term)
    : JZTerminalToolBase("terminal_open", term)
{
    vector<ParamSchema> params;
    params.push_back(JZCodeTool::makeParam("cwd", ParamType::String, "初始工作目录(相对工作区或绝对); 空=工作区根"));
    setToolDefine(makeDef("terminal_open", "本地单用户终端, 默认会话已就绪(无需显式 open)。", params, {}));
}

void JZTerminalOpenTool::call(const ToolCall &params)
{
    if (!m_term)
    {
        finish(params, false, "终端未初始化");
        return;
    }
    string id = m_term->id();
    string cwd = cwdOf(m_term);
    string text = "当前为单用户终端, 默认会话已就绪\nsession: " + (id.empty() ? string("(默认)") : id);
    if (!cwd.empty())
        text += "\n[cwd] " + cwd;
    finish(params, true, text);
}

JZTerminalListTool::JZTerminalListTool(JZVirtualTerminal *term)
    : JZTerminalToolBase("terminal_list", term)
{
    setToolDefine(makeDef("terminal_list", "列出本地单用户终端的会话(只有一个)。", {}, {}));
}

void JZTerminalListTool::call(const ToolCall &params)
{
    if (!m_term)
    {
        finish(params, false, "终端未初始化");
        return;
    }
    JZVirtualTerminal::SessionInfo info = m_term->info();
    string text = "session: " + (info.id.empty() ? string("(默认)") : info.id) + "\n";
    text += "  cwd: " + info.cwd + "\n";
    text += "  状态: ";
    if (info.runningCount > 0)
        text += "运行中(" + to_string(info.runningCount) + " 条在跑)";
    else
        text += "空闲";
    text += "\n";
    finish(params, true, text);
}

JZTerminalCloseTool::JZTerminalCloseTool(JZVirtualTerminal *term)
    : JZTerminalToolBase("terminal_close", term)
{
    setToolDefine(makeDef("terminal_close", "本地单用户终端常驻, 无需关闭。", {}, {}));
}

void JZTerminalCloseTool::call(const ToolCall &params)
{
    if (!m_term)
    {
        finish(params, false, "终端未初始化");
        return;
    }
    finish(params, true, "当前为单用户终端, 终端常驻, 无需关闭");
}
