#include "JZTerminalTools.h"

#include <vector>

#include "JZVirtualTerminal.h"

using namespace std;

// 这几条约定在每个 tool 的 description 里都要出现一次 —— 模型的 shell 直觉
// 在这里只有一部分成立, description 是它唯一的说明书。
static const char *kShellCaveats =
    "注意: 每条命令在一个**新进程**里执行(没有常驻 shell)。"
    "只有单独一条 `cd <目录>` 会改变后续命令的目录; `cd a && b` 里的 cd 只在那一条内有效。"
    "环境变量不跨命令保留, 要就内联写(`FOO=bar <命令>`)。";

// ==========================================================================
// 基类
// ==========================================================================
JZTerminalToolBase::JZTerminalToolBase(JZVirtualTerminal *term)
    : m_term(term)
{
}

bool JZTerminalToolBase::sessionOf(const ToolCall &call, string &id, string &error,
                                   bool createIfNone)
{
    id.clear();

    if (!m_term)
    {
        error = "工具没有绑定虚拟终端";
        return false;
    }

    string want = call.params.get("session", "").asString();
    if (!want.empty())
    {
        JZVirtualTerminal::SessionInfo info;
        if (!m_term->sessionInfo(want, info))
        {
            error = "没有这个会话: " + want + " (用 terminal_list 看有哪些)";
            return false;
        }
        id = want;
        return true;
    }

    // 没指定就用现有会话(模型不该为了跑一条命令先管理会话 id)
    vector<JZVirtualTerminal::SessionInfo> all = m_term->sessions();
    if (!all.empty())
    {
        id = all.begin()->id;
        return true;
    }

    if (!createIfNone)
    {
        error = "还没有任何会话, 先用 terminal_open 开一个";
        return false;
    }

    id = m_term->openSession(string(), error);
    return !id.empty();
}

// ==========================================================================
// terminal_run
// ==========================================================================
JZTerminalRunTool::JZTerminalRunTool(JZVirtualTerminal *term)
    : JZTerminalToolBase(term)
{
    ToolDefine def;
    def.name = "terminal_run";
    def.description =
        "在这台机器上执行一条命令, 返回它的输出(合并了 stderr)和退出码。"
        "命令**整条交给 shell**, 所以管道(|)、重定向(> >>)、串联(&& || ;)、"
        "通配符(*)、变量($VAR) 都按 shell 的语义工作。"
        "常用命令: ls / cat / mkdir / rm / cp / mv / grep / find / sed / git / cmake / make。"
        "不支持: 后台 `&`、nohup/setsid/screen、`$( )`、反引号、heredoc、进程替换;"
        "交互式程序(vim/top/REPL)也跑不了。"
        "长任务记得传 timeout_ms(默认 120 秒), 超时会强制终止。";
    def.description += string("\n") + kShellCaveats;
    def.parameters.push_back(makeParam("command", ParamType::String, "要执行的完整命令"));
    def.parameters.push_back(makeParam("timeout_ms", ParamType::Number,
                                       "超时毫秒数, 默认 120000, 超时强制终止"));
    def.parameters.push_back(makeParam("session", ParamType::String,
                                       "可选, 在哪个会话里跑; 不传就用当前会话"));
    def.required_parameters.push_back("command");
    setToolDefine(def);
}

void JZTerminalRunTool::call(const ToolCall &params)
{
    if (!m_term)
    {
        finish(params, false, "工具没有绑定虚拟终端");
        return;
    }
    if (!m_pending.id.empty())
    {
        finish(params, false, "上一条命令还没结束, 等它回来再发");
        return;
    }

    string id;
    string error;
    if (!sessionOf(params, id, error))
    {
        finish(params, false, error);
        return;
    }

    string command = params.params.get("command", "").asString();
    int timeoutMs = params.params.get("timeout_ms", 120000).asInt();
    if (timeoutMs < 1000)
        timeoutMs = 120000;

    // 回调只捕获 id, 不捕获 ToolCall 引用(那条调用可能已经被取消/释放)
    string reqId = params.id;
    bool started = m_term->run(id, command, timeoutMs,
                               [this, reqId](bool ok, const string &out, const string &err)
                               {
                                   onRunDone(reqId, ok, out, err);
                               }, error);
    if (!started)
    {
        finish(params, false, error);
        return;
    }

    m_pending = params;
    m_runningSession = id;
}

void JZTerminalRunTool::onRunDone(const string &reqId, bool ok, const string &out,
                                  const string &err)
{
    ToolCall call = m_pending;
    if (call.id != reqId)
        return;                       // 已经被取消或换了另一条命令, 结果丢弃

    string session = m_runningSession;
    m_pending = ToolCall();
    m_runningSession.clear();

    string text = out;
    if (!err.empty())
    {
        if (!text.empty() && text.back() != '\n')
            text += "\n";
        text += "[错误] " + err;
    }
    if (text.empty())
        text = ok ? "(无输出)" : err;

    // 带上当前 cwd: 链式 cd 不跨命令保留, 模型得知道自己现在到底在哪,
    // 否则它会猜"我刚才 cd 过了"。
    string cwd;
    if (m_term && !session.empty() && m_term->printDir(session, cwd) && !cwd.empty())
        text += "\n[cwd] " + cwd;

    finish(call, ok, text);
}

void JZTerminalRunTool::cancel(const string &id)
{
    if (id != m_pending.id)
        return;

    string session = m_runningSession;
    m_pending = ToolCall();
    m_runningSession.clear();

    if (m_term && !session.empty())
        m_term->cancel(session);      // 停掉在跑的命令; 它的应答会被上面的 id 比对丢掉
}

// ==========================================================================
// terminal_open / terminal_list / terminal_close
// ==========================================================================
JZTerminalOpenTool::JZTerminalOpenTool(JZVirtualTerminal *term)
    : JZTerminalToolBase(term)
{
    ToolDefine def;
    def.name = "terminal_open";
    def.description =
        "开一个新的会话(一个独立的工作目录 + 各自的在跑命令), 返回会话 id。"
        "一般**不需要**手动开 —— terminal_run 会自己用一个默认会话。"
        "只有要在两个目录之间来回切换时才值得开多个。";
    def.parameters.push_back(makeParam("cwd", ParamType::String,
                                       "可选, 会话的工作目录(默认工作区根)"));
    setToolDefine(def);
}

void JZTerminalOpenTool::call(const ToolCall &params)
{
    if (!m_term)
    {
        finish(params, false, "工具没有绑定虚拟终端");
        return;
    }

    string error;
    string id = m_term->openSession(params.params.get("cwd", "").asString(), error);
    if (id.empty())
    {
        finish(params, false, error.empty() ? "开会话失败" : error);
        return;
    }

    string cwd;
    m_term->printDir(id, cwd);
    finish(params, true, "会话 id: " + id + "\n[cwd] " + cwd);
}

JZTerminalListTool::JZTerminalListTool(JZVirtualTerminal *term)
    : JZTerminalToolBase(term)
{
    ToolDefine def;
    def.name = "terminal_list";
    def.description = "列出当前所有会话(会话 id / 工作目录 / 是否正在跑命令)。";
    setToolDefine(def);
}

void JZTerminalListTool::call(const ToolCall &params)
{
    if (!m_term)
    {
        finish(params, false, "工具没有绑定虚拟终端");
        return;
    }

    vector<JZVirtualTerminal::SessionInfo> all = m_term->sessions();
    if (all.empty())
    {
        finish(params, true, "(还没有会话; 直接 terminal_run 会自动开一个)");
        return;
    }

    string text;
    for (const JZVirtualTerminal::SessionInfo &s : all)
    {
        text += s.id + "  " + s.cwd + (s.running ? "  [运行中]" : "") + "\n";
    }
    finish(params, true, text);
}

JZTerminalCloseTool::JZTerminalCloseTool(JZVirtualTerminal *term)
    : JZTerminalToolBase(term)
{
    ToolDefine def;
    def.name = "terminal_close";
    def.description = "关掉一个会话(在它里面跑的命令会被终止)。不用的会话不必手动关, "
                      "空闲一段时间会自动回收。";
    def.parameters.push_back(makeParam("session", ParamType::String, "要关的会话 id"));
    def.required_parameters.push_back("session");
    setToolDefine(def);
}

void JZTerminalCloseTool::call(const ToolCall &params)
{
    if (!m_term)
    {
        finish(params, false, "工具没有绑定虚拟终端");
        return;
    }

    string id;
    string error;
    // 这里**不要**顺手创建一个: 关一个不存在的会话没意义
    if (!sessionOf(params, id, error, false))
    {
        finish(params, false, error);
        return;
    }

    if (!m_term->closeSession(id))
    {
        finish(params, false, "关闭失败: " + id);
        return;
    }
    finish(params, true, "已关闭会话 " + id);
}
