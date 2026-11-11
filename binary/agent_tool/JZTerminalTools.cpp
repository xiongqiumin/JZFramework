#include "JZTerminalTools.h"

#include "JZPlatform.h"
#include "JZStd.h"      // str_toLower
#include "JZString.h"

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
    // 这里不再 setToolDefine: 每个派生类构造时都会定义自己的 description/参数,
    // 基类再写一份只会被覆盖(以前那句"deprecated, 请走 agent_node"还会误导人 ——
    // 这 6 个工具正是 code_agent / agent_cli 的本地主执行面)。
    (void)name;
}

// ======================================================================
// JZTerminalRunTool
// ======================================================================
JZTerminalRunTool::JZTerminalRunTool(JZVirtualTerminal *term)
    : JZTerminalToolBase("terminal_run", term)
{
    vector<ParamSchema> params;
    params.push_back(JZCodeTool::makeParam("command", ParamType::String, "要执行的命令"));
    params.push_back(JZCodeTool::makeParam("timeout_ms", ParamType::Number,
                                           "超时毫秒(默认 120000; 到点强杀)"));
    setToolDefine(makeDef("terminal_run",
                          "在沙箱终端执行一条 shell 命令(本地 code_agent 单用户终端)。"
                          "终端**有状态**: cd / export 会保留到下一条命令。\n"
                          "同一时刻只能跑一条命令(单前台任务); 等它跑完再发下一条, "
                          "长任务用 timeout_ms 控制。",
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
    // 参数被我们改写了就在回包里说一句 —— 静默改写会让模型以为"设了 500ms 就 500ms"
    string paramNote;
    if (timeoutMs < 1000)
    {
        paramNote = "\n[提示] timeout_ms=" + to_string(timeoutMs) + " 太小, 已按 120000 ms 处理";
        timeoutMs = 120000;
    }

    // 命令被拒时 run() 会**同步**回调一次(ok=false), 用它记一下, 免得下面再回一次结果。
    auto fired = std::make_shared<bool>(false);
    auto cb = [this, params, fired, paramNote](bool ok, const string &out, const string &err)
    {
        // 同步那次(命令被拒)已经回过包了: 再 finish 一次就是重复回包
        if (*fired)
            return;
        *fired = true;
        string text = out;
        if (!err.empty())
        {
            if (!text.empty() && text.back() != '\n')
                text += "\n";
            text += "[错误] " + err;
        }
        if (text.empty())
            text = ok ? "(无输出)" : err;
        // ⚠ 回包**就是命令的原样输出**: 不再追加 [cwd] / [run_id] 之类的位置与提示
        //   (2026-10-14 按用户要求改成"不加工": 那些尾巴对读输出没帮助, 反而把噪音喂进上下文)。
        //   唯一保留的附加信息是上面那条失败原因(非 0 退出码 / 超时被杀) —— isOk 不进模型上下文
        //   (见 JZAgent 只发 tool 消息的 content), 不写这一行模型就无从知道命令失败了。
        text += paramNote;
        finish(params, ok, text);
    };

    JZResult<void> started = m_term->run(command, timeoutMs, cb);
    if (started.isError())
    {
        if (!*fired)
            finish(params, false, started.errorMessage());
        return;
    }
}

void JZTerminalRunTool::cancel(const std::string &id)
{
    if (!m_term)
        return;
    // 单前台任务: 停的就是当前这一条(没有"别的在跑命令"可误伤)
    (void)id;
    m_term->cancel();
}

// ======================================================================
// JZTerminalCancelTool
// ======================================================================
JZTerminalCancelTool::JZTerminalCancelTool(JZVirtualTerminal *term)
    : JZTerminalToolBase("terminal_cancel", term)
{
    setToolDefine(makeDef("terminal_cancel",
                          "停掉当前正在跑的那条命令(单前台任务, 一次只有一条)。\n"
                          "`cmd &` 起的后台进程不在前台任务里, 要停它用 `kill <pid>`。",
                          {}, {}));
}

void JZTerminalCancelTool::call(const ToolCall &params)
{
    if (!m_term)
    {
        finish(params, false, "终端未初始化");
        return;
    }

    if (!m_term->cancel())
    {
        finish(params, false, "当前没有在跑的命令");
        return;
    }
    finish(params, true, "已请求停止当前命令 —— 下一次分片检查时杀掉进程");
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
