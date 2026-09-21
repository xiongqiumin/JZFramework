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

    // 解析目标会话: 远程节点是多租户(每个调用方先 terminal_open 拿到自己的会话),
    // 为避免多人串同一终端 / 串 cwd / 串后台任务, 这里**强制要求显式 session** ——
    // 必须先 terminal_open 拿到 id 再传进来; 不传或不存在都报错, 不自动取/自动建。
    JZVirtualTerminal *resolveSession(JZTerminalRegistry *reg, const Json::Value &args,
                                     std::string &sid, std::string &error)
    {
        if (!reg)
        {
            error = "工具未绑定终端注册表";
            return nullptr;
        }
        std::string want = args.get("session", "").asString();
        if (want.empty())
        {
            error = "必须先 terminal_open 开一个会话, 再把返回的 session id 填到 session 参数";
            return nullptr;
        }
        JZVirtualTerminal *term = reg->terminal(want, error);
        if (!term)
        {
            error = "没有这个会话: " + want + " (用 terminal_list 看有哪些, 或先 terminal_open)";
            return nullptr;
        }
        sid = want;
        return term;
    }

    std::string cwdOf(JZVirtualTerminal *term)
    {
        std::string cwd;
        if (term && term->printDir(cwd) && !cwd.empty())
            return cwd;
        return std::string();
    }

    // 从客户端参数里取整数: 客户端(尤其模型生成的 tool call)可能把"该是整数"的字段
    // 传成字符串 / null / 数组, 而 JsonCpp 的 asInt() 在类型不可转换时会抛 Json::LogicError,
    // 这个异常没被接住会直接 terminate 掉整个节点进程。这里改成"不可转换就用默认",
    // 让脏参数降级成合理行为而不是崩溃。
    int jIntArg(const Json::Value &args, const char *name, int def)
    {
        const Json::Value &v = args.get(name, def);
        if (v.isConvertibleTo(Json::intValue))
            return v.asInt();
        return def;
    }
}

// ======================================================================
// JZTerminalRunMcpTool
// ======================================================================
JZTerminalRunMcpTool::JZTerminalRunMcpTool(JZTerminalRegistry *reg)
    : m_registry(reg)
{
    setName("terminal_run");
    setDescription(
        "⚠ 远程节点是多租户(每个调用方先 terminal_open 开自己的会话): 本工具**必须先 terminal_open 开一个会话**, "
        "把返回的 session id 填到下面的 session 参数; 不传 session 会直接报错。\n"
        "在沙箱终端执行一条 shell 命令(语法由服务器解析: 支持 && || ; 串联、| 管道、"
        "> >> < 重定向、通配符与变量展开)。\n"
        "默认同步执行, 返回合并输出 + 退出信息。\n"
        "⭐ 同一个会话可以**同时跑多条命令**(终端侧并发, 各跑各的): 一条长任务不会堵住别的命令。\n"
        "每次调用返回一个 [run_id], 用 terminal_query 按 run_id 查它; 客户端对在跑的请求发取消\n"
        "(notifications/cancelled)只会停那一条, 不影响同会话里其它在跑的命令。\n"
        "可选:\n"
        "  - stream=true    : 边跑边按 notifications/message 流式推日志(POST 内联 SSE), 结束再推结果。\n"
        "  - background=true : 后台运行, 立即返回 run_id(不流式), 之后用 terminal_query 取日志。\n"
        "  - stream_filter  : \"stdout\"/\"stderr\"/\"all\", 过滤推送/查询的流(默认 all)。\n"
        "  - grep          : 只保留含该关键字的行(流式为子串匹配, 查询为整行匹配)。\n"
        "  - error_context : 输出过滤开关(默认 true, 所有命令都截断, 编译命令特殊处理)。"
        "非编译命令超长时保留头 50 + 尾 50 行、中间省略; 编译命令成功取尾部 20 行、"
        "失败取开头约 100 行错误(错误块完整); 完整日志仍可用 terminal_query 取。"
        "设 false 则原样回传全量。\n"
        "超时后输出已产出的部分并报告超时原因。");
    Json::Value p(Json::objectValue);
    p["type"] = "object";
    Json::Value props(Json::objectValue);
    props["command"] = Json::objectValue;
    props["command"]["type"] = "string";
    props["command"]["description"] = "要执行的命令";
    props["session"] = Json::objectValue;
    props["session"]["type"] = "string";
    props["session"]["description"] = "目标会话 id(**必须**: 先 terminal_open 开一个会话, 把返回的 id 填这里; 不填会报错)";
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
    props["error_context"] = Json::objectValue;
    props["error_context"]["type"] = "boolean";
    props["error_context"]["description"] = "构建命令输出做错误截取(默认 true; false=原样全量)";
    p["properties"] = props;
    p["required"] = Json::arrayValue;
    p["required"].append("command");
    setInputSchema(p);
}

void JZTerminalRunMcpTool::run(const Json::Value &id, const Json::Value &args)
{
    if (!m_registry)
    {
        sigFinished.emit(id, mcpTextResult("工具未绑定终端注册表", true));
        return;
    }

    std::string command = args.get("command", "").asString();
    if (command.empty())
    {
        sigFinished.emit(id, mcpTextResult("command 不能为空", true));
        return;
    }
    bool background = args.get("background", false).asBool();
    bool stream = args.get("stream", false).asBool();
    std::string streamFilter = args.get("stream_filter", "all").asString();
    std::string grep = args.get("grep", "").asString();
    int timeoutMs = jIntArg(args, "timeout_ms", 120000);
    if (timeoutMs < 1000)
        timeoutMs = 120000;

    std::string sid, error;
    JZVirtualTerminal *term = resolveSession(m_registry, args, sid, error);
    if (!term)
    {
        sigFinished.emit(id, mcpTextResult(error, true));
        return;
    }
    // 输出过滤开关(默认开): 按次给(终端能同时跑多条, 用终端级开关会互相串)
    JZVirtualTerminal::OutputFilter filter = args.get("error_context", true).asBool()
                                                 ? JZVirtualTerminal::OutputFilter::On
                                                 : JZVirtualTerminal::OutputFilter::Off;

    // ---- 后台模式: 启动即返回 run_id, 不流式 ----
    if (background)
    {
        JZResult<std::string> started = term->run(command, timeoutMs,
                                                  [](bool, const std::string &, const std::string &) {},
                                                  nullptr, true, filter);
        if (started.isError())
        {
            sigFinished.emit(id, mcpTextResult(started.errorMessage(), true));
            return;
        }
        std::string text = "已后台启动(只返回 id, 日志用 terminal_query 取)\nrun_id: " + started.value();
        std::string cwd = cwdOf(term);
        if (!cwd.empty())
            text += "\n[cwd] " + cwd;
        sigFinished.emit(id, mcpTextResult(text, false));
        return;
    }

    // ---- 前台(可选流式) ----
    // run_id 拿到的时间点比回调晚(命令被拒时回调会**同步**跑在 run() 里, 那时还没有 run_id),
    // 所以放共享变量里, 回调按它取值; fired 用来避免"同步回调 + 下面再回一次"发两遍结果。
    auto ridPtr = std::make_shared<std::string>();
    auto fired = std::make_shared<bool>(false);
    JZResult<std::string> started = term->run(command, timeoutMs,
                                              [this, id, ridPtr, fired, term](bool ok, const std::string &out, const std::string &err)
                                              {
                                                  *fired = true;
                                                  onRunDone(id, ok, out, err, *ridPtr, term);
                                              },
                                              stream ? std::function<void(const std::string &, const std::string &)>(
                                                  [this, id, streamFilter, grep](const std::string &stream, const std::string &chunk)
                                                  { emitChunk(id, stream, chunk, streamFilter, grep); })
                                                     : nullptr,
                                              false, filter);
    if (started.isError())
    {
        if (!*fired)
            sigFinished.emit(id, mcpTextResult(started.errorMessage(), true));
        return;
    }
    *ridPtr = started.value();
    Inflight info;
    info.session = sid;
    info.runId = *ridPtr;
    m_inflight[id.asString()] = info;
}

void JZTerminalRunMcpTool::onRunDone(const Json::Value &id, bool ok, const std::string &out,
                                     const std::string &err, const std::string &runId, JZVirtualTerminal *term)
{
    m_inflight.erase(id.asString());

    std::string text = out;
    if (!err.empty())
    {
        if (!text.empty() && text.back() != '\n')
            text += "\n";
        text += "[错误] " + err;
    }
    if (text.empty())
        text = ok ? "(无输出)" : err;

    std::string cwd = cwdOf(term);
    if (!cwd.empty())
        text += "\n[cwd] " + cwd;
    if (!runId.empty())
        text += "\n[run_id] " + runId + "   (用 terminal_query 取完整/历史日志)";

    sigFinished.emit(id, mcpTextResult(text, !ok));
}

void JZTerminalRunMcpTool::emitChunk(const Json::Value &id, const std::string &stream, const std::string &chunk,
                                     const std::string &streamFilter, const std::string &grep)
{
    if (chunk.empty())
        return;
    if (streamFilter == "stdout" && stream != "stdout")
        return;
    if (streamFilter == "stderr" && stream != "stderr")
        return;
    if (!grep.empty() && chunk.find(grep) == std::string::npos)
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
    auto it = m_inflight.find(id.asString());
    if (it == m_inflight.end())
        return;
    std::string session = it->second.session;
    std::string runId = it->second.runId;
    m_inflight.erase(it);

    std::string error;
    JZVirtualTerminal *term = m_registry ? m_registry->terminal(session, error) : nullptr;
    if (!term)
        return;

    // 只停这一条(同会话里别的在跑命令不动)。run_id 为空(理论上不该有)就退回全停。
    if (!runId.empty())
        term->cancel(runId);
    else
        term->cancelAll();
}

// ======================================================================
// JZTerminalQueryMcpTool
// ======================================================================
JZTerminalQueryMcpTool::JZTerminalQueryMcpTool(JZTerminalRegistry *reg)
    : m_registry(reg)
{
    setName("terminal_query");
    setDescription(
        "取后台/历史运行的日志(由 terminal_run 留下的运行记录)。\n"
        "  - run_id : 取指定运行(缺省时必须带 session, 取该会话最近一次运行;\n"
        "             若该会话还有命令在跑, 优先给最近那条**还在跑**的, 并列出全部在跑的 run_id)\n"
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
    props["session"]["description"] = "会话 id(缺省时必须带 run_id)";
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
    if (!m_registry)
    {
        sigFinished.emit(id, mcpTextResult("工具未绑定终端注册表", true));
        return;
    }

    std::string runId = args.get("run_id", "").asString();
    std::string session = args.get("session", "").asString();
    int tail = jIntArg(args, "tail", 0);
    std::string stream = args.get("stream", "all").asString();
    std::string grep = args.get("grep", "").asString();

    JZVirtualTerminal::RunRecordInfo rec;
    JZVirtualTerminal *termPtr = nullptr;   // 按 session 查时才有: 末尾列出"还有哪些在跑"
    if (!runId.empty())
    {
        if (!m_registry->runSnapshot(runId, rec))
        {
            sigFinished.emit(id, mcpTextResult("没有这个 run_id: " + runId, true));
            return;
        }
    }
    else
    {
        if (session.empty())
        {
            sigFinished.emit(id, mcpTextResult("没有 run_id 时, 必须带 session 取该会话最近一次运行", true));
            return;
        }
        std::string error;
        JZVirtualTerminal *term = m_registry->terminal(session, error);
        if (!term)
        {
            sigFinished.emit(id, mcpTextResult(error, true));
            return;
        }
        termPtr = term;
        std::vector<std::string> ids = term->runIds();
        if (ids.empty())
        {
            sigFinished.emit(id, mcpTextResult("该会话没有运行记录(先用 terminal_run 跑过)", false));
            return;
        }
        // 并发之后"最近一次"有多个候选: 优先给最近那条**还在跑**的(模型多半就是想知道
        // 它跑到哪了), 没有在跑的就退回最后一条记录。
        runId = ids.back();
        for (auto rit = ids.rbegin(); rit != ids.rend(); ++rit)
        {
            JZVirtualTerminal::RunRecordInfo tmp;
            if (term->runSnapshot(*rit, tmp) && tmp.status == "running")
            {
                runId = *rit;
                break;
            }
        }
        term->runSnapshot(runId, rec);
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
    // 该会话还有别的命令在跑时一并列出: 并发下"我在等的那条"和"还在跑的别的条"很容易混
    if (termPtr)
    {
        std::vector<std::string> live = termPtr->activeRunIds();
        if (!live.empty())
        {
            text += "该会话在跑 " + std::to_string(live.size()) + " 条:";
            for (const std::string &rid : live)
                text += " " + rid;
            text += "\n";
        }
    }
    if (!rec.error.empty())
        text += "[结束] " + rec.error + "\n";
    text += "---- 日志(" + std::to_string(lines.size()) + " 行) ----\n";
    for (const auto &l : lines)
        text += l.stream + ": " + l.text + "\n";

    sigFinished.emit(id, mcpTextResult(text, false));
}

// ======================================================================
// JZTerminalOpenMcpTool
// ======================================================================
JZTerminalOpenMcpTool::JZTerminalOpenMcpTool(JZTerminalRegistry *reg)
    : m_registry(reg)
{
    setName("terminal_open");
    setDescription("开一个终端会话(一个独立终端实例, 各自 cwd), 返回会话 id。之后用这个 id 调 terminal_run / terminal_query。");
    Json::Value p(Json::objectValue);
    p["type"] = "object";
    Json::Value props(Json::objectValue);
    props["cwd"] = Json::objectValue;
    props["cwd"]["type"] = "string";
    props["cwd"]["description"] = "初始工作目录(相对工作区或绝对); 空=工作区根";
    p["properties"] = props;
    setInputSchema(p);
}

void JZTerminalOpenMcpTool::run(const Json::Value &id, const Json::Value &args)
{
    if (!m_registry)
    {
        sigFinished.emit(id, mcpTextResult("工具未绑定终端注册表", true));
        return;
    }
    std::string cwd = args.get("cwd", "").asString();
    std::string error;
    std::string sid = m_registry->open(cwd, error);
    if (sid.empty())
    {
        sigFinished.emit(id, mcpTextResult(error, true));
        return;
    }
    std::string text = "已开会话\nsession: " + sid;
    std::string ccwd = cwdOf(m_registry->terminal(sid, error));
    if (!ccwd.empty())
        text += "\n[cwd] " + ccwd;
    sigFinished.emit(id, mcpTextResult(text, false));
}

// ======================================================================
// JZTerminalListMcpTool
// ======================================================================
JZTerminalListMcpTool::JZTerminalListMcpTool(JZTerminalRegistry *reg)
    : m_registry(reg)
{
    setName("terminal_list");
    setDescription("列出当前所有终端会话(id / cwd / 在跑几条命令)。");
    Json::Value p(Json::objectValue);
    p["type"] = "object";
    p["properties"] = Json::objectValue;
    setInputSchema(p);
}

void JZTerminalListMcpTool::run(const Json::Value &id, const Json::Value &args)
{
    (void)args;
    if (!m_registry)
    {
        sigFinished.emit(id, mcpTextResult("工具未绑定终端注册表", true));
        return;
    }
    std::vector<JZVirtualTerminal::SessionInfo> list = m_registry->list();
    if (list.empty())
    {
        sigFinished.emit(id, mcpTextResult("还没有会话(用 terminal_open 开一个)", false));
        return;
    }
    std::string text;
    for (const auto &s : list)
    {
        text += "session: " + s.id + "\n";
        text += "  cwd: " + s.cwd + "\n";
        text += "  状态: ";
        if (s.runningCount > 0)
            text += "运行中(" + std::to_string(s.runningCount) + " 条在跑)";
        else
            text += "空闲";
        text += "\n";
    }
    sigFinished.emit(id, mcpTextResult(text, false));
}

// ======================================================================
// JZTerminalCloseMcpTool
// ======================================================================
JZTerminalCloseMcpTool::JZTerminalCloseMcpTool(JZTerminalRegistry *reg)
    : m_registry(reg)
{
    setName("terminal_close");
    setDescription("关掉一个终端会话(对应的终端被销毁, 在跑命令会被终止)。");
    Json::Value p(Json::objectValue);
    p["type"] = "object";
    Json::Value props(Json::objectValue);
    props["session"] = Json::objectValue;
    props["session"]["type"] = "string";
    props["session"]["description"] = "要关闭的会话 id(来自 terminal_open)";
    p["properties"] = props;
    p["required"] = Json::arrayValue;
    p["required"].append("session");
    setInputSchema(p);
}

void JZTerminalCloseMcpTool::run(const Json::Value &id, const Json::Value &args)
{
    if (!m_registry)
    {
        sigFinished.emit(id, mcpTextResult("工具未绑定终端注册表", true));
        return;
    }
    std::string sid = args.get("session", "").asString();
    if (sid.empty())
    {
        sigFinished.emit(id, mcpTextResult("session 不能为空", true));
        return;
    }
    if (m_registry->close(sid))
        sigFinished.emit(id, mcpTextResult("已关闭会话: " + sid, false));
    else
        sigFinished.emit(id, mcpTextResult("没有这个会话: " + sid, true));
}
