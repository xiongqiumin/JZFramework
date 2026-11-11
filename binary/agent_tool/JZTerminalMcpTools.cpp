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
        "⭐ **单前台任务**: 一个会话同时只能跑一条命令, 这条命令退出才回包(超时强杀)。\n"
        "返回合并输出 + 退出信息(没有 run_id / 后台模式 / 查询工具)。要后台就在命令里写 `&`:\n"
        "输出跟命令自己的重定向走(`> log 2>&1`), 之后用 cat / read_file 看。\n"
        "客户端对在跑的请求发取消(notifications/cancelled)只停这一条。");
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
    // ---- 单前台任务: 没有后台模式, 也没有增量流式(JZShell 整条返回) ----
    // fired 用来避免"同步回调(命令被拒) + 下面再回一次"发两遍结果。
    auto fired = std::make_shared<bool>(false);
    JZResult<void> started = term->run(command, timeoutMs,
                                       [this, id, fired, term](bool ok, const std::string &out, const std::string &err)
                                       {
                                           *fired = true;
                                           onRunDone(id, ok, out, err, term);
                                       });
    if (started.isError())
    {
        if (!*fired)
            sigFinished.emit(id, mcpTextResult(started.errorMessage(), true));
        return;
    }
    m_inflight[id.asString()] = sid;
}

void JZTerminalRunMcpTool::onRunDone(const Json::Value &id, bool ok, const std::string &out,
                                     const std::string &err, JZVirtualTerminal *term)
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

    sigFinished.emit(id, mcpTextResult(text, !ok));
}

void JZTerminalRunMcpTool::cancel(const Json::Value &id)
{
    auto it = m_inflight.find(id.asString());
    if (it == m_inflight.end())
        return;
    std::string session = it->second;
    m_inflight.erase(it);

    std::string error;
    JZVirtualTerminal *term = m_registry ? m_registry->terminal(session, error) : nullptr;
    if (term)
        term->cancel();
}

// ======================================================================
// JZTerminalOpenMcpTool
// ======================================================================
JZTerminalOpenMcpTool::JZTerminalOpenMcpTool(JZTerminalRegistry *reg)
    : m_registry(reg)
{
    setName("terminal_open");
    setDescription("开一个终端会话(一个独立终端实例, 各自 cwd), 返回会话 id。之后用这个 id 调 terminal_run。");
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
