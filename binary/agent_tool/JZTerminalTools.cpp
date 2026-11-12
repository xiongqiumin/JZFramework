#include "JZTerminalTools.h"

#include "JZPlatform.h"
#include "JZStd.h"      // str_toLower
#include "JZString.h"

using namespace std;

namespace
{
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
JZTerminalToolBase::JZTerminalToolBase(const std::string &name, JZTerminalPool *pool)
    : m_pool(pool)
{
    // 这里不再 setToolDefine: 每个派生类构造时都会定义自己的 description/参数,
    // 基类再写一份只会被覆盖。
    (void)name;
}

// ======================================================================
// JZTerminalRunTool
// ======================================================================
JZTerminalRunTool::JZTerminalRunTool(JZTerminalPool *pool)
    : JZTerminalToolBase("terminal_run", pool)
{
    vector<ParamSchema> params;
    params.push_back(JZCodeTool::makeParam("command", ParamType::String, "要执行的命令"));
    params.push_back(JZCodeTool::makeParam("timeout_ms", ParamType::Number,
                                           "超时毫秒(默认 120000; 到点强杀)"));
    setToolDefine(makeDef("terminal_run",
                          "在沙箱终端执行一条 shell 命令(本地单用户终端)。\n"
                          "⭐ **执行是无状态的**: 每条命令都从工作区根开始, `cd` 只对**这一条**有效"
                          "(下一条又回根), `export` / 变量 / 函数 / alias 同样过不到下一条。"
                          "要进子目录就在同一条命令里写 `cd 子目录 && ...`;"
                          "要带环境变量就写 `VAR=x cmd`。\n"
                          "⭐ **并发**: 多条命令可以同时跑, 每条各自占一台终端; 到并发上限会直接报忙。\n"
                          "长任务用 timeout_ms 控制; 要停就调 terminal_cancel。",
                          params, { "command" }));
}

void JZTerminalRunTool::call(const ToolCall &params)
{
    if (!m_pool)
    {
        finish(params, false, "终端池未初始化");
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

    // 借一台终端: 并发调用各占一台, 互不干扰
    string termError;
    JZVirtualTerminal *term = m_pool->acquire(string(), termError);
    if (!term)
    {
        finish(params, false, termError.empty() ? "拿不到终端" : termError);
        return;
    }

    // 命令被拒时 run() 会**同步**回调一次(ok=false), 用它记一下, 免得下面再回一次结果。
    auto fired = std::make_shared<bool>(false);
    auto cb = [this, params, fired, paramNote, term](bool ok, const string &out, const string &err)
    {
        // 同步那次(命令被拒)已经回过包了: 再 finish 一次就是重复回包
        if (*fired)
            return;
        *fired = true;
        if (m_pool)
            m_pool->release(term);   // 先还再用: 回调之后这台就归别人了
        string text = out;
        if (!err.empty())
        {
            if (!text.empty() && text.back() != '\n')
                text += "\n";
            text += "[错误] " + err;
        }
        if (text.empty())
            text = ok ? "(无输出)" : err;
        // 回包基本就是命令的原样输出(2026-10-14 定的"不加工"), 只保留失败原因 ——
        // isOk 不进模型上下文(见 JZAgent 只发 tool 消息的 content), 不写这行模型就
        // 无从知道命令失败了。
        text += paramNote;
        finish(params, ok, text);
    };

    JZResult<void> started = term->run(command, timeoutMs, cb);
    if (started.isError())
    {
        if (!*fired)
        {
            m_pool->release(term);
            finish(params, false, started.errorMessage());
        }
        return;
    }
}

void JZTerminalRunTool::cancel(const std::string &id)
{
    (void)id;
    if (m_pool)
        m_pool->cancelAll();      // 停掉所有在跑的(可能有多条并发)
}

// ======================================================================
// JZTerminalCancelTool
// ======================================================================
JZTerminalCancelTool::JZTerminalCancelTool(JZTerminalPool *pool)
    : JZTerminalToolBase("terminal_cancel", pool)
{
    setToolDefine(makeDef("terminal_cancel",
                          "停掉正在跑的命令(并发时**一起停**所有在跑的, 不需要参数)。\n"
                          "`cmd &` 起的后台进程不在前台任务里, 要停它用 `kill <pid>`。",
                          {}, {}));
}

void JZTerminalCancelTool::call(const ToolCall &params)
{
    if (!m_pool)
    {
        finish(params, false, "终端池未初始化");
        return;
    }

    const int n = m_pool->cancelAll();
    if (n <= 0)
    {
        finish(params, false, "当前没有在跑的命令");
        return;
    }
    finish(params, true, "已请求停止 " + to_string(n) + " 条命令 —— 下一次分片检查时杀掉进程");
}

// terminal_open / terminal_list / terminal_close 已删除(2026-11-12):
// 终端改成无状态执行, 没有"会话"这个概念了 —— 见 JZTerminalTools.h 末尾的说明。
