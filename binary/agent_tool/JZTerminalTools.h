#ifndef JZ_TERMINAL_TOOLS_H_
#define JZ_TERMINAL_TOOLS_H_

// 本地(单用户)终端工具: 每个 agent 持**一个终端池**, 每次 terminal_run 借一台、
// 跑完归还 —— 多条命令可以真并发, 每条各自独占一台终端。
// 共享的执行/审核逻辑都在 JZVirtualTerminal 里, 这里只是把它包成 JZCodeTool。

#include "JZCodeTools.h"      // JZCodeTool / ToolCall / ParamType / makeParam
#include "JZVirtualTerminal.h"
#include "json/json.h"

class JZTerminalPool;

class JZTerminalToolBase : public JZCodeTool
{
public:
    JZTerminalToolBase(const std::string &name, JZTerminalPool *pool);
    void setPool(JZTerminalPool *pool) { m_pool = pool; }
    JZTerminalPool *pool() const { return m_pool; }

protected:
    JZTerminalPool *m_pool = nullptr;
};

// terminal_run(本地版): 同 agent_node 的 terminal_run, 直接在 code_agent 进程里跑。
// ⭐ 单前台任务指的是"一台终端一次一条"; 并发由池里多台终端提供。
class JZTerminalRunTool : public JZTerminalToolBase
{
public:
    explicit JZTerminalRunTool(JZTerminalPool *pool);
    void call(const ToolCall &params) override;
    void cancel(const std::string &id) override;
};

// terminal_cancel: 停掉正在跑的(可能有多条并发, 一起停, 不需要参数)。
class JZTerminalCancelTool : public JZTerminalToolBase
{
public:
    explicit JZTerminalCancelTool(JZTerminalPool *pool);
    void call(const ToolCall &params) override;
};

// ⛔ terminal_open / terminal_list / terminal_close 已删除(2026-11-12):
//   终端改成**无状态执行** —— 每条命令都从工作区根重来, 没有"会话"要开/查/关。

#endif // !JZ_TERMINAL_TOOLS_H_
