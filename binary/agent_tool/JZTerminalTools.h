#ifndef JZ_TERMINAL_TOOLS_H_
#define JZ_TERMINAL_TOOLS_H_

// 本地(单用户)终端工具: 每个 code_agent 持**一个** JZVirtualTerminal(单终端),
// session 参数在这里被忽略(单用户一个终端, 行为与之前"自动开默认会话"等价)。
// 共享的执行/审核逻辑都在 JZVirtualTerminal 里, 这里只是把它包成 JZCodeTool。

#include "JZCodeTools.h"      // JZCodeTool / ToolCall / ParamType / makeParam
#include "JZVirtualTerminal.h"
#include "json/json.h"

class JZVirtualTerminal;

class JZTerminalToolBase : public JZCodeTool
{
public:
    JZTerminalToolBase(const std::string &name, JZVirtualTerminal *term);
    void setTerminal(JZVirtualTerminal *term) { m_term = term; }
    JZVirtualTerminal *terminal() const { return m_term; }

protected:
    JZVirtualTerminal *m_term = nullptr;
};

// terminal_run(本地版): 同 agent_node 的 terminal_run, 但直接在 code_agent 进程里跑,
// 单用户终端、自动用默认会话, 不需要显式 open。
// 可以并发: 同一个终端同时跑多条命令, 每次调用各自一个 run_id(取消按 run_id 精确停)。
// background=true 时走 runBackground: 启动成功即返回 run_id, 不等命令结束、不设超时
//   —— 起服务这类自己不退出的命令用它; 之后用 terminal_query 查、terminal_cancel 停。
class JZTerminalRunTool : public JZTerminalToolBase
{
public:
    explicit JZTerminalRunTool(JZVirtualTerminal *term);
    void call(const ToolCall &params) override;
    void cancel(const std::string &id) override;

private:
    // 在跑的调用: tool call id -> run_id(用于 cancel 精确停那一条)
    std::map<std::string, std::string> m_runs;
};

// terminal_query: 按 run_id 查一次运行的状态与日志(前台跑完的也能查 —— 取完整输出;
// 输出已不做任何过滤/截断, 它只是"能反复看、能按 grep / stream / tail 挑")。run_id 省略 = 最近一次运行。
// ⭐ 轮询类(见 JZAgentTool::isPollTool): 模型查后台任务进度会**反复**用同样的参数调它,
//   那是正常用法, 不该被循环检测当打转打掉。
class JZTerminalQueryTool : public JZTerminalToolBase
{
public:
    explicit JZTerminalQueryTool(JZVirtualTerminal *term);
    void call(const ToolCall &params) override;
    bool isPollTool() const override { return true; }
};

// terminal_cancel: 按 run_id 停掉一条还在跑的命令(后台起的 long-running 任务靠它收尾)。
// run_id 省略 = 最近一条在跑的。
class JZTerminalCancelTool : public JZTerminalToolBase
{
public:
    explicit JZTerminalCancelTool(JZVirtualTerminal *term);
    void call(const ToolCall &params) override;
};

class JZTerminalOpenTool : public JZTerminalToolBase
{
public:
    explicit JZTerminalOpenTool(JZVirtualTerminal *term);
    void call(const ToolCall &params) override;
};

class JZTerminalListTool : public JZTerminalToolBase
{
public:
    explicit JZTerminalListTool(JZVirtualTerminal *term);
    void call(const ToolCall &params) override;
    bool isPollTool() const override { return true; }   // 查询类: 反复看会话是正常用法
};

class JZTerminalCloseTool : public JZTerminalToolBase
{
public:
    explicit JZTerminalCloseTool(JZVirtualTerminal *term);
    void call(const ToolCall &params) override;
};

#endif // !JZ_TERMINAL_TOOLS_H_
