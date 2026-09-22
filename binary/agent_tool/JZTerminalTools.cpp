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

    // 省略 run_id 时用的默认值: 查历史取最近一次运行, 停命令取最近一条在跑的
    string defaultRunId(JZVirtualTerminal *term)
    {
        vector<string> ids = term->runIds();
        return ids.empty() ? string() : ids.back();
    }

    string defaultActiveRunId(JZVirtualTerminal *term)
    {
        vector<string> ids = term->activeRunIds();
        return ids.empty() ? string() : ids.back();
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
    params.push_back(JZCodeTool::makeParam("timeout_ms", ParamType::Number,
                                           "超时毫秒(默认 120000; 到点强杀。background=true 时忽略)"));
    params.push_back(JZCodeTool::makeParam("background", ParamType::Boolean,
                                           "后台运行(默认 false): 启动成功就立即返回 run_id, 不等它结束、"
                                           "也不设超时 —— 起服务/常驻进程这类自己不退出的命令用它, "
                                           "之后用 terminal_query 查状态与日志、terminal_cancel 停掉"));
    params.push_back(JZCodeTool::makeParam("error_context", ParamType::Boolean,
                                           "输出过滤开关(默认 true, 所有命令都截断, 编译命令特殊处理): "
                                           "非编译超长保留头50+尾50行; 编译成功取尾部20行、失败取开头约100行错误"
                                           "(默认 true; false=原样全量)"));
    setToolDefine(makeDef("terminal_run",
                          "在沙箱终端执行一条 shell 命令(本地 code_agent 单用户终端)。"
                          "可以同时跑多条: 一条长任务不会堵住别的命令(每条各自一个 run_id)。\n"
                          "默认等命令自己退出才回结果(到 timeout_ms 强杀); "
                          "background=true 时启动成功就返回 run_id, 不等它结束、不设超时。",
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
    bool background = p.get("background", false).asBool();
    int timeoutMs = p.get("timeout_ms", 120000).asInt();
    // 参数被我们改写了就在回包里说一句 —— 静默改写会让模型以为"设了 500ms 就 500ms"
    string paramNote;
    if (background && p.isMember("timeout_ms"))
        paramNote = "\n[提示] background=true 时没有超时, timeout_ms 被忽略(要限时请前台跑)";
    else if (!background && timeoutMs < 1000)
    {
        paramNote = "\n[提示] timeout_ms=" + to_string(timeoutMs) + " 太小, 已按 120000 ms 处理";
        timeoutMs = 120000;
    }

    // 输出过滤开关(默认开): 按次给(终端能同时跑多条, 用终端级开关会互相串)
    JZVirtualTerminal::OutputFilter filter = p.get("error_context", true).asBool()
                                                 ? JZVirtualTerminal::OutputFilter::On
                                                 : JZVirtualTerminal::OutputFilter::Off;

    // 命令被拒时 run() 会**同步**回调一次(ok=false), 用它记一下, 免得下面再回一次结果。
    // ridPtr: run_id 要等 run() 返回才有, 而回调可能发生在 run() 里(同步那次), 所以放共享变量。
    auto fired = std::make_shared<bool>(false);
    auto ridPtr = std::make_shared<string>();
    auto cb = [this, params, fired, ridPtr, paramNote](bool ok, const string &out, const string &err)
    {
        // 后台任务: tool call 启动那一步已经回过包了, 进程结束时这次回调直接丢掉 ——
        // 结果进运行记录, 由 terminal_query 去取(再 finish 一次就是重复回包)。
        if (*fired)
            return;
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
        if (!ridPtr->empty())
            text += "\n[run_id] " + *ridPtr + "   (完整/历史日志用 terminal_query 取)";
        text += paramNote;
        finish(params, ok, text);
    };

    JZResult<string> started = background
                                   ? m_term->runBackground(command, cb, filter)
                                   : m_term->run(command, timeoutMs, cb, nullptr, false, filter);
    if (started.isError())
    {
        if (!*fired)
            finish(params, false, started.errorMessage());
        return;
    }

    // 后台: 启动成功就直接回 run_id。
    // ⚠ 不记 m_runs: 这条 tool call 到此就结束了, 它的取消只能走 terminal_cancel(run_id) ——
    //   JZCodeTool::cancel(id) 是按 tool call id 的, 那条路对后台任务没有意义。
    if (background)
    {
        string text = "已后台启动(不等它结束, 不设超时)\n[run_id] " + started.value() +
                      "\n用 terminal_query 查状态与日志(默认只回新增行), 用 terminal_cancel 停掉它";
        string cwd = cwdOf(m_term);
        if (!cwd.empty())
            text += "\n[cwd] " + cwd;
        text += paramNote;
        finish(params, true, text);
        return;
    }

    // 记下 run_id: 取消时按它精确停这一条(见 cancel)
    m_runs[params.id] = started.value();
    *ridPtr = started.value();
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
// JZTerminalQueryTool
// ======================================================================
JZTerminalQueryTool::JZTerminalQueryTool(JZVirtualTerminal *term)
    : JZTerminalToolBase("terminal_query", term)
{
    vector<ParamSchema> params;
    params.push_back(JZCodeTool::makeParam("run_id", ParamType::String,
                                           "要查的运行 id(terminal_run 返回的 [run_id]); 省略 = 最近一次运行"));
    params.push_back(JZCodeTool::makeParam("tail", ParamType::Number,
                                           "最多回多少行(默认 200; 0 = 不限制, 仍受回包字节上限约束)"));
    params.push_back(JZCodeTool::makeParam("grep", ParamType::String,
                                           "只保留含该关键字的行(空 = 不过滤)"));
    params.push_back(JZCodeTool::makeParam("stream", ParamType::String,
                                           "按流过滤: stdout / stderr / all(默认)"));
    params.push_back(JZCodeTool::makeParam("from_last", ParamType::Boolean,
                                           "默认 true = 只回**上次查询之后新增**的行(轮询查进度用, 省 token); "
                                           "false = 看历史(回最后 tail 行, 不改变增量位置)"));
    setToolDefine(makeDef("terminal_query",
                          "按 run_id 查一次运行的状态与日志。\n"
                          "不带过滤条件时默认只回**自上次查询之后新增**的行(这条运行第一次查时回最近一段) ——\n"
                          "反复查进度不会把同一段日志重复灌进上下文; 想看历史/从头看给 from_last=false。\n"
                          "带 grep / stream 时按\"在整条日志里找\"处理(不受增量限制)。\n"
                          "后台起的(background=true)和前台跑完的都能查 —— 这里给的是**完整日志**, "
                          "不受 terminal_run 回包时的截断影响。\n"
                          "还在跑的: 用它可以反复查进度, 不用干等(轮询不算重复调用)。",
                          params, {}));
}

void JZTerminalQueryTool::call(const ToolCall &params)
{
    if (!m_term)
    {
        finish(params, false, "终端未初始化");
        return;
    }

    Json::Value p = params.params;
    string runId = p.get("run_id", "").asString();
    if (runId.empty())
    {
        runId = defaultRunId(m_term);
        if (runId.empty())
        {
            finish(params, false, "这个终端还没有运行过任何命令(没有可查的 run_id)");
            return;
        }
    }
    int tail = p.get("tail", 200).asInt();
    string grep = p.get("grep", "").asString();
    string stream = str_toLower(p.get("stream", "").asString());
    if (stream == "all" || stream == "*")
        stream.clear();
    // 默认"接着上次看": 轮询查进度时只回新增行, 不把同一段日志反复灌进上下文
    const bool fromLast = p.get("from_last", true).asBool();

    JZVirtualTerminal::RunRecordInfo info;
    if (!m_term->runSnapshot(runId, info))
    {
        // 顺手把可用的 id 给它: 后台任务跑久了, run_id 记错很常见
        vector<string> ids = m_term->runIds();
        string hint;
        for (size_t i = ids.size() > 5 ? ids.size() - 5 : 0; i < ids.size(); i++)
            hint += (hint.empty() ? " 最近的 run_id: " : ", ") + ids[i];
        finish(params, false, "没有这个 run_id: " + runId + hint);
        return;
    }

    string text;
    text += "[run_id] " + info.runId + "  [" + info.status + "]";
    if (info.status == "running")
    {
        int64_t ms = JZPlatform::getTickCountMs() - info.startTime;
        text += "  已运行 " + to_string(ms / 1000) + "s";
    }
    else
    {
        text += "  exit=" + to_string(info.exitCode);
        if (info.timedOut)
            text += " (超时被杀)";
    }
    if (info.background)
        text += "  [后台]";
    if (info.pid > 0)
        text += "  pid=" + to_string(info.pid);
    text += "\n[command] " + info.command;
    if (!info.error.empty())
        text += "\n[结果] " + info.error;

    // ---- 选行: 先按流/关键字过滤, 再按模式切区间 ----
    // abs = 该行在**整条运行**里的绝对行号(从第一行算起, 不受"记录上限丢头"影响) ——
    // 增量游标就建在它上面, 所以跨查询对得上。
    struct Pick
    {
        size_t abs;
        const JZVirtualTerminal::RunLogLine *line;
    };
    vector<Pick> sel;
    for (size_t i = 0; i < info.log.size(); i++)
    {
        const JZVirtualTerminal::RunLogLine &l = info.log[i];
        if (!stream.empty() && l.stream != stream)
            continue;
        if (!grep.empty() && l.text.find(grep) == string::npos)
            continue;
        Pick k;
        k.abs = info.firstLine + i;
        k.line = &l;
        sel.push_back(k);
    }

    // 增量模式 = 这条运行之前被查过(readPos > 0) **且没带过滤条件**。
    // 带 grep/stream 的查询是"在整条日志里找东西"⇒ 按看历史处理, 并且不动增量游标。
    const bool incremental = fromLast && info.readPos > 0 && grep.empty() && stream.empty();
    size_t from = 0;
    size_t to = sel.size();
    if (incremental)
    {
        size_t cursor = info.readPos > info.firstLine ? info.readPos : info.firstLine;
        from = sel.size();
        for (size_t i = 0; i < sel.size(); i++)
        {
            if (sel[i].abs >= cursor)
            {
                from = i;
                break;
            }
        }
        to = (tail > 0 && sel.size() > from + (size_t)tail) ? from + (size_t)tail : sel.size();
    }
    else if (tail > 0 && sel.size() > (size_t)tail)
    {
        from = sel.size() - (size_t)tail;   // 只看最后 tail 行
    }

    // ---- 组装回包(带字节上限) ----
    // 模型上下文比磁盘贵得多: 单次回包卡在 32KB, 超了就地停并告诉它还有多少行。
    const size_t kMaxQueryBytes = 32 * 1024;
    string body;
    size_t shown = 0;
    for (size_t i = from; i < to; i++)
    {
        string one = sel[i].line->stream + "| " + sel[i].line->text + "\n";
        if (body.size() + one.size() > kMaxQueryBytes)
            break;
        body += one;
        shown++;
    }

    text += "\n[日志 ";
    if (shown == 0)
        text += "0 行";
    else
        text += to_string(shown) + " 行(第 " + to_string(sel[from].abs + 1) + ".." +
                to_string(sel[from + shown - 1].abs + 1) + " 行, 累计 " + to_string(info.totalLines) + " 行)";
    if (!grep.empty())
        text += ", grep=" + grep;
    if (!stream.empty())
        text += ", stream=" + stream;
    if (incremental)
        text += ", 只看上次之后的新增";
    text += "]";

    if (incremental)
    {
        if (from + shown < to)
            text += "\n[还有 " + to_string(to - from - shown) + " 行未显示, 再查一次接着看]";
        if (info.readPos > info.firstLine)
            text += "\n[提示] 更早的 " + to_string(info.readPos - info.firstLine) +
                    " 行没读到过, 已被记录上限丢弃(想要长日志请用 grep 缩小范围)";
    }
    else
    {
        if (from > 0)
            text += "\n[更早的 " + to_string(from) + " 行未显示; 要全看: from_last=false 且 tail=0]";
        if (info.firstLine > 0)
            text += "\n[提示] 记录只留最近 " + to_string(info.log.size()) + " 行, 更早的 " +
                    to_string(info.firstLine) + " 行已丢弃";
    }

    if (shown == 0)
    {
        if (incremental && info.status == "running")
            text += "\n(暂无新增输出, 任务还在跑)";
        else if (incremental)
            text += "\n(没有新增输出, 任务已结束)";
        else if (!grep.empty() || !stream.empty())
            text += "\n(没有符合过滤条件的行)";
        else
            text += "\n(暂无输出" + (info.status == "running" ? string(", 还在跑") : string()) + ")";
    }
    else
    {
        text += "\n" + body;
    }
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
        text.pop_back();   // body 末尾多出来的换行去掉

    // 游标前移: 只把**真正回给模型的行**记成已读 ⇒ 被 tail/字节上限截掉的下一轮还能拿到
    if (incremental)
    {
        if (shown > 0)
            m_term->advanceRunReadPos(runId, sel[from + shown - 1].abs + 1);
    }
    else if (fromLast)
    {
        // 第一次查(看的是最近一段): 记成"已看过最新的", 下一次开始就是增量
        m_term->advanceRunReadPos(runId, info.totalLines);
    }

    finish(params, true, text);
}

// ======================================================================
// JZTerminalCancelTool
// ======================================================================
JZTerminalCancelTool::JZTerminalCancelTool(JZVirtualTerminal *term)
    : JZTerminalToolBase("terminal_cancel", term)
{
    vector<ParamSchema> params;
    params.push_back(JZCodeTool::makeParam("run_id", ParamType::String,
                                           "要停的运行 id; 省略 = 最近一条在跑的"));
    setToolDefine(makeDef("terminal_cancel",
                          "停掉一条还在跑的命令(后台起的 long-running 任务靠它收尾)。\n"
                          "只停这一条, 同终端里别的在跑命令不受影响; 收尾结果用 terminal_query 查。",
                          params, {}));
}

void JZTerminalCancelTool::call(const ToolCall &params)
{
    if (!m_term)
    {
        finish(params, false, "终端未初始化");
        return;
    }

    Json::Value p = params.params;
    string runId = p.get("run_id", "").asString();
    if (runId.empty())
    {
        runId = defaultActiveRunId(m_term);
        if (runId.empty())
        {
            finish(params, false, "没有在跑的命令(没有可停的 run_id)");
            return;
        }
    }

    if (!m_term->cancel(runId))
    {
        finish(params, false, "这条已经不在跑了: " + runId +
                                  "(它可能已结束 —— 用 terminal_query 查它的结果)");
        return;
    }
    finish(params, true, "已请求停止 " + runId +
                             " —— 进程被杀, 收尾结果(退出码/日志)用 terminal_query 查");
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

    // 在跑的都列出来(run_id + 已跑多久 + 命令): 模型忘了 run_id 时不用靠 query 报错才拿到
    vector<string> active = m_term->activeRunIds();
    if (!active.empty())
    {
        text += "  在跑:\n";
        int64_t now = JZPlatform::getTickCountMs();
        for (size_t i = 0; i < active.size(); i++)
        {
            JZVirtualTerminal::RunRecordInfo rec;
            if (!m_term->runSnapshot(active[i], rec))
                continue;
            string cmd = rec.command.size() > 60 ? rec.command.substr(0, 60) + "…" : rec.command;
            text += "    " + rec.runId + "  " + to_string((now - rec.startTime) / 1000) + "s  " + cmd +
                    "\n";
        }
    }

    // 最近的历史(不管在不在跑): 查日志时挑 run_id 用
    vector<string> all = m_term->runIds();
    if (!all.empty())
    {
        text += "  最近运行:\n";
        for (size_t i = all.size() > 5 ? all.size() - 5 : 0; i < all.size(); i++)
        {
            JZVirtualTerminal::RunRecordInfo rec;
            if (!m_term->runSnapshot(all[i], rec))
                continue;
            text += "    " + rec.runId + "  [" + rec.status + "]";
            if (rec.status != "running")
                text += "  exit=" + to_string(rec.exitCode);
            if (rec.background)
                text += "  [后台]";
            text += "\n";
        }
    }
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
