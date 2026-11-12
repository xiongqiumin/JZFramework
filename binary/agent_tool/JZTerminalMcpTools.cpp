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
JZTerminalRunMcpTool::JZTerminalRunMcpTool(JZTerminalPool *pool)
    : m_pool(pool)
{
    setName("terminal_run");
    setDescription(
        "在沙箱终端执行一条 shell 命令(语法由服务器解析: 支持 && || ; 串联、| 管道、"
        "> >> < 重定向、通配符与变量展开)。\n"
        "⭐ **执行是无状态的**: 每条命令都从工作区根开始, `cd` 只对**这一条**有效"
        "(下一条又回根), `export` / 变量 / 函数 / alias 同样过不到下一条。"
        "要进子目录就在同一条命令里写 `cd 子目录 && ...`; 要带环境变量就写 `VAR=x cmd`。\n"
        "⭐ **并发**: 同时有多条命令在跑也可以, 每条各自占一台终端、互不影响;"
        "并发到上限会直接报忙(不排队)。\n"
        "返回合并输出 + 退出信息(没有 run_id / 后台模式 / 查询工具)。要后台就在命令里写 `&`:\n"
        "输出跟命令自己的重定向走(`> log 2>&1`), 之后用 cat / read_file 看。\n"
        "客户端对在跑的请求发取消(notifications/cancelled)只停这一条。");
    Json::Value p(Json::objectValue);
    p["type"] = "object";
    Json::Value props(Json::objectValue);
    props["command"] = Json::objectValue;
    props["command"]["type"] = "string";
    props["command"]["description"] = "要执行的命令";
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
    if (!m_pool)
    {
        sigFinished.emit(id, mcpTextResult("终端池未初始化", true));
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

    // 每次调用借一台: 多条同时跑就是多台, 各自独立(不会互相抢执行槽)
    std::string termError;
    JZVirtualTerminal *term = m_pool->acquire(std::string(), termError);
    if (!term)
    {
        sigFinished.emit(id, mcpTextResult(termError.empty() ? "拿不到终端" : termError, true));
        return;
    }
    // ---- 没有后台模式, 也没有增量流式(JZShell 整条返回) ----
    // fired 用来避免"同步回调(命令被拒) + 下面再回一次"发两遍结果。
    auto fired = std::make_shared<bool>(false);
    JZResult<void> started = term->run(command, timeoutMs,
                                       [this, id, fired, term](bool ok, const std::string &out, const std::string &err)
                                       {
                                           *fired = true;
                                           if (m_pool)
                                               m_pool->release(term);   // 先还再用: 回调之后这台就归别人了
                                           onRunDone(id, ok, out, err);
                                       });
    if (started.isError())
    {
        if (!*fired)
        {
            m_pool->release(term);
            sigFinished.emit(id, mcpTextResult(started.errorMessage(), true));
        }
        return;
    }
    m_inflight[id.asString()] = term;
}

void JZTerminalRunMcpTool::onRunDone(const Json::Value &id, bool ok, const std::string &out,
                                     const std::string &err)
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

    sigFinished.emit(id, mcpTextResult(text, !ok));
}

void JZTerminalRunMcpTool::cancel(const Json::Value &id)
{
    // 按 call id 精确停"我自己借的那台" —— 池化之后别人正在用的终端不能碰
    auto it = m_inflight.find(id.asString());
    if (it == m_inflight.end())
        return;
    JZVirtualTerminal *term = it->second;
    m_inflight.erase(it);
    if (term)
        term->cancel();     // 收尾仍走 run 的回调(那里会 release)
}

/* terminal_open / terminal_list / terminal_close 已删除(2026-11-12):
   执行改成无状态后, 这三个工具做不了任何事 —— open 改变不了环境(每条命令都从基线重来),
   list 只会回一条, close 会把正在用的终端连根拔掉。模型却常常先 open 再 run, 白绕一步。 */


