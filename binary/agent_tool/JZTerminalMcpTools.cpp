#include "JZTerminalMcpTools.h"
#include "JZVirtualTerminal.h"

#include <memory>

namespace
{
    // 构造一个 MCP text 结果(与 bridge 中一致)
    Json::Value mcpTextResult(const std::string &text, bool isError)
    {
        Json::Value result(Json::objectValue);
        Json::Value content(Json::arrayValue);
        Json::Value item(Json::objectValue);
        item["type"] = "text";
        item["text"] = text;
        content.append(item);
        result["content"] = content;
        result["isError"] = isError;
        return result;
    }

    // 解析目标会话: 指定且存在 → 用之; 否则取默认会话; 都没有则开一个。
    bool resolveSession(JZVirtualTerminal *term, const Json::Value &args, std::string &sid, std::string &error)
    {
        if (!term)
        {
            error = "工具未绑定终端";
            return false;
        }
        std::string want = args.get("session", "").asString();
        if (!want.empty())
        {
            JZVirtualTerminal::SessionInfo info;
            if (!term->sessionInfo(want, info))
            {
                error = "没有这个会话: " + want;
                return false;
            }
            sid = want;
            return true;
        }
        std::vector<JZVirtualTerminal::SessionInfo> all = term->sessions();
        if (!all.empty())
        {
            sid = all.begin()->id;
            return true;
        }
        sid = term->openSession(std::string(), error);
        return !sid.empty();
    }
}

// ======================================================================
// JZTerminalRunMcpTool
// ======================================================================
JZTerminalRunMcpTool::JZTerminalRunMcpTool(JZVirtualTerminal *term)
    : m_term(term)
{
    setName("terminal_run");
    setDescription(
        "在沙箱终端执行一条 shell 命令(语法由服务器解析: 支持 && || ; 串联、| 管道、"
        "> >> < 重定向、通配符与变量展开)。\n"
        "默认同步执行, 返回合并输出 + 退出信息。\n"
        "可选:\n"
        "  - stream=true    : 边跑边按 notifications/message 流式推日志(POST 内联 SSE), 结束再推结果。\n"
        "  - background=true : 后台运行, 立即返回 run_id(不流式), 之后用 terminal_query 取日志。\n"
        "  - stream_filter  : \"stdout\"/\"stderr\"/\"all\", 过滤推送/查询的流(默认 all)。\n"
        "  - grep          : 只保留含该关键字的行(流式为子串匹配, 查询为整行匹配)。\n"
        "超时后输出已产出的部分并报告超时原因。");
    Json::Value p(Json::objectValue);
    p["type"] = "object";
    Json::Value props(Json::objectValue);
    props["command"] = Json::objectValue;
    props["command"]["type"] = "string";
    props["command"]["description"] = "要执行的命令";
    props["session"] = Json::objectValue;
    props["session"]["type"] = "string";
    props["session"]["description"] = "目标会话 id(缺省用默认会话, 没有则新建)";
    props["timeout_ms"] = Json::objectValue;
    props["timeout_ms"]["type"] = "integer";
    props["timeout_ms"]["description"] = "超时毫秒(默认 120000)";
    props["stream"] = Json::objectValue;
    props["stream"]["type"] = "boolean";
    props["stream"]["description"] = "是否流式推送日志(内联 SSE)";
    props["background"] = Json::objectValue;
    props["background"]["type"] = "boolean";
    props["background"]["description"] = "是否后台运行(立即返回 run_id)";
    props["stream_filter"] = Json::objectValue;
    props["stream_filter"]["type"] = "string";
    props["stream_filter"]["description"] = "流过滤: stdout/stderr/all";
    props["grep"] = Json::objectValue;
    props["grep"]["type"] = "string";
    props["grep"]["description"] = "关键字过滤(见说明)";
    p["properties"] = props;
    p["required"] = Json::arrayValue;
    p["required"].append("command");
    setInputSchema(p);
}

void JZTerminalRunMcpTool::run(const Json::Value &id, const Json::Value &args)
{
    if (!m_term)
    {
        sigFinished.emit(id, mcpTextResult("工具未绑定终端", true));
        return;
    }

    std::string command = args.get("command", "").asString();
    if (command.empty())
    {
        sigFinished.emit(id, mcpTextResult("command 不能为空", true));
        return;
    }
    if (m_running)
    {
        sigFinished.emit(id, mcpTextResult("上一条命令还没结束, 等它回来再发", true));
        return;
    }

    bool background = args.get("background", false).asBool();
    bool stream = args.get("stream", false).asBool();
    m_streamFilter = args.get("stream_filter", "all").asString();
    m_grep = args.get("grep", "").asString();
    int timeoutMs = args.get("timeout_ms", 120000).asInt();
    if (timeoutMs < 1000)
        timeoutMs = 120000;

    std::string sid, error;
    if (!resolveSession(m_term, args, sid, error))
    {
        sigFinished.emit(id, mcpTextResult(error, true));
        return;
    }

    // ---- 后台模式: 启动即返回 run_id, 不流式 ----
    if (background)
    {
        auto ridPtr = std::make_shared<std::string>();
        bool started = m_term->run(sid, command, timeoutMs,
                                   [](bool, const std::string &, const std::string &) {}, error,
                                   ridPtr.get(), nullptr, true);
        if (!started)
        {
            sigFinished.emit(id, mcpTextResult(error, true));
            return;
        }
        std::string text = "已后台启动(只返回 id, 日志用 terminal_query 取)\nrun_id: " + *ridPtr;
        std::string cwd;
        if (m_term->printDir(sid, cwd) && !cwd.empty())
            text += "\n[cwd] " + cwd;
        sigFinished.emit(id, mcpTextResult(text, false));
        return;
    }

    // ---- 前台(可选流式) ----
    auto ridPtr = std::make_shared<std::string>();
    bool started = m_term->run(sid, command, timeoutMs,
                               [this, id, ridPtr, sid](bool ok, const std::string &out, const std::string &err)
                               { onRunDone(id, ok, out, err, *ridPtr, sid); },
                               error, ridPtr.get(),
                               stream ? std::function<void(const std::string &, const std::string &)>(
                                   [this, id](const std::string &stream, const std::string &chunk)
                                   { emitChunk(id, stream, chunk); })
                                      : nullptr,
                               false);
    if (!started)
    {
        sigFinished.emit(id, mcpTextResult(error, true));
        return;
    }
    m_running = true;
    m_runningSession = sid;
}

void JZTerminalRunMcpTool::onRunDone(const Json::Value &id, bool ok, const std::string &out,
                                     const std::string &err, const std::string &runId, const std::string &session)
{
    m_running = false;
    m_runningSession.clear();

    std::string text = out;
    if (!err.empty())
    {
        if (!text.empty() && text.back() != '\n')
            text += "\n";
        text += "[错误] " + err;
    }
    if (text.empty())
        text = ok ? "(无输出)" : err;

    std::string cwd;
    if (m_term && !session.empty() && m_term->printDir(session, cwd) && !cwd.empty())
        text += "\n[cwd] " + cwd;
    if (!runId.empty())
        text += "\n[run_id] " + runId + "   (用 terminal_query 取完整/历史日志)";

    sigFinished.emit(id, mcpTextResult(text, !ok));
}

void JZTerminalRunMcpTool::emitChunk(const Json::Value &id, const std::string &stream, const std::string &chunk)
{
    if (chunk.empty())
        return;
    if (m_streamFilter == "stdout" && stream != "stdout")
        return;
    if (m_streamFilter == "stderr" && stream != "stderr")
        return;
    if (!m_grep.empty() && chunk.find(m_grep) == std::string::npos)
        return;

    Json::Value note(Json::objectValue);
    note["jsonrpc"] = "2.0";
    note["method"] = "notifications/message";
    Json::Value params(Json::objectValue);
    params["level"] = "info";
    params["logger"] = "terminal";
    params["data"] = chunk;
    note["params"] = params;
    sigNotify.emit(id, note);
}

void JZTerminalRunMcpTool::cancel(const Json::Value &id)
{
    if (!m_running)
        return;
    std::string session = m_runningSession;
    m_running = false;
    m_runningSession.clear();
    if (m_term && !session.empty())
        m_term->cancel(session);
}

// ======================================================================
// JZTerminalQueryMcpTool
// ======================================================================
JZTerminalQueryMcpTool::JZTerminalQueryMcpTool(JZVirtualTerminal *term)
    : m_term(term)
{
    setName("terminal_query");
    setDescription(
        "取后台/历史运行的日志(由 terminal_run 留下的运行记录)。\n"
        "  - run_id : 取指定运行(缺省时取该会话/全局最近一次运行)\n"
        "  - tail   : 只取最近 N 行(0=全部)\n"
        "  - stream : stdout/stderr/all 流过滤\n"
        "  - grep   : 只保留含该关键字的整行");
    Json::Value p(Json::objectValue);
    p["type"] = "object";
    Json::Value props(Json::objectValue);
    props["run_id"] = Json::objectValue;
    props["run_id"]["type"] = "string";
    props["run_id"]["description"] = "运行 id(来自 terminal_run 的 [run_id] 行)";
    props["session"] = Json::objectValue;
    props["session"]["type"] = "string";
    props["session"]["description"] = "会话 id(缺省时全局最近一次运行)";
    props["tail"] = Json::objectValue;
    props["tail"]["type"] = "integer";
    props["tail"]["description"] = "只取最近 N 行(0=全部)";
    props["stream"] = Json::objectValue;
    props["stream"]["type"] = "string";
    props["stream"]["description"] = "流过滤: stdout/stderr/all";
    props["grep"] = Json::objectValue;
    props["grep"]["type"] = "string";
    props["grep"]["description"] = "关键字过滤";
    p["properties"] = props;
    setInputSchema(p);
}

void JZTerminalQueryMcpTool::run(const Json::Value &id, const Json::Value &args)
{
    if (!m_term)
    {
        sigFinished.emit(id, mcpTextResult("工具未绑定终端", true));
        return;
    }

    std::string runId = args.get("run_id", "").asString();
    std::string session = args.get("session", "").asString();
    int tail = args.get("tail", 0).asInt();
    std::string stream = args.get("stream", "all").asString();
    std::string grep = args.get("grep", "").asString();

    JZVirtualTerminal::RunRecordInfo rec;
    if (runId.empty())
    {
        // 取最近一次运行: 指定会话范围内, 否则全局
        std::vector<std::string> ids = m_term->runIds(session);
        if (ids.empty())
        {
            sigFinished.emit(id, mcpTextResult("没有运行记录(先用 terminal_run 跑过)", false));
            return;
        }
        runId = ids.back();
    }
    if (!m_term->runSnapshot(runId, rec))
    {
        sigFinished.emit(id, mcpTextResult("没有这个 run_id: " + runId, true));
        return;
    }

    // 过滤 + 取最近 tail 行
    std::vector<JZVirtualTerminal::RunLogLine> lines;
    for (const auto &l : rec.log)
    {
        if (stream == "stdout" && l.stream != "stdout")
            continue;
        if (stream == "stderr" && l.stream != "stderr")
            continue;
        if (!grep.empty() && l.text.find(grep) == std::string::npos)
            continue;
        lines.push_back(l);
    }
    if (tail > 0 && (int)lines.size() > tail)
        lines = std::vector<JZVirtualTerminal::RunLogLine>(lines.end() - tail, lines.end());

    std::string text;
    text += "run_id: " + rec.runId + "\n";
    text += "command: " + rec.command + "\n";
    text += "status: " + rec.status;
    if (rec.status == "done")
        text += "  exit=" + std::to_string(rec.exitCode) + (rec.timedOut ? " (超时)" : "");
    text += "\n";
    if (!rec.error.empty())
        text += "[结束] " + rec.error + "\n";
    text += "---- 日志(" + std::to_string(lines.size()) + " 行) ----\n";
    for (const auto &l : lines)
        text += l.stream + ": " + l.text + "\n";

    sigFinished.emit(id, mcpTextResult(text, false));
}
