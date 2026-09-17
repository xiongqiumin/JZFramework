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
class JZTerminalRunTool : public JZTerminalToolBase
{
public:
    explicit JZTerminalRunTool(JZVirtualTerminal *term);
    void call(const ToolCall &params) override;
    void cancel(const std::string &id) override;
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
};

class JZTerminalCloseTool : public JZTerminalToolBase
{
public:
    explicit JZTerminalCloseTool(JZVirtualTerminal *term);
    void call(const ToolCall &params) override;
};

#endif // !JZ_TERMINAL_TOOLS_H_
