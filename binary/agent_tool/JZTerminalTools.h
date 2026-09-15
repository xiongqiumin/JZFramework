#ifndef JZ_TERMINAL_TOOLS_H_
#define JZ_TERMINAL_TOOLS_H_

#include <string>

#include "JZCodeTools.h"

class JZVirtualTerminal;

// ---------------------------------------------------------------------------
// 虚拟终端的工具层 —— 把 JZVirtualTerminal 暴露给模型
//
// 为什么单独一层, 而不让 JZVirtualTerminal 直接继承 JZAgentTool:
//   终端是"执行面 + 审核面", 工具是"给模型看的名字与参数"。分开之后,
//   本地 code_agent(JZAgentTool) 与远端 agent_node(MCP 工具) 共用同一份终端实现,
//   各自取各自的名字。
//
// ⚠ 下面这些约定必须写进工具的 description —— 模型对 shell 的直觉在这里
//   **只有一部分**成立, 不写清它一定会踩:
//   1. 每条命令是一个**新进程**, 没有常驻 shell。
//   2. 只有**单独一条** `cd <目录>` 会改变后续命令的目录; `cd a && b` 里的 cd
//      只在那一条命令内有效(`terminal_run` 的结果里会回一行 `[cwd]`, 不用猜)。
//   3. 环境变量不跨命令保留, 要用就内联写(`FOO=bar <命令>`)。
//   4. 不支持: 后台 `&`、nohup/setsid/screen、`$( )`、反引号、heredoc、进程替换。
//   5. 交互式程序(vim/top/REPL)做不了 —— 子进程的 stdin/stdout 是管道, 不是 tty。
// ---------------------------------------------------------------------------
class JZTerminalToolBase : public JZCodeTool
{
public:
    explicit JZTerminalToolBase(JZVirtualTerminal *term = nullptr);

    // 非拥有: 宿主(本地 code_agent / 远端 agent_node)持有终端
    void setTerminal(JZVirtualTerminal *term) { m_term = term; }
    JZVirtualTerminal *terminal() const { return m_term; }

protected:
    // 取会话 id: 参数里指定了就用它; 没指定就用现有会话; 一个都没有就开一个默认的。
    // (模型不该为了跑一条命令先去管理会话 id —— 多会话是可选能力, 不是必经步骤)
    bool sessionOf(const ToolCall &call, std::string &id, std::string &error,
                   bool createIfNone = true);

    JZVirtualTerminal *m_term = nullptr;
};

// ------------------------------------------------------------------
// terminal_run: 执行一条命令。命令**整条交给 shell**, 支持管道/重定向/&& 串联。
//   异步: 命令跑完(或超时/被杀)才回结果。
// ------------------------------------------------------------------
class JZTerminalRunTool : public JZTerminalToolBase
{
public:
    explicit JZTerminalRunTool(JZVirtualTerminal *term = nullptr);

    virtual void call(const ToolCall &params) override;
    virtual void cancel(const std::string &id) override;

private:
    void onRunDone(const std::string &reqId, bool ok, const std::string &out,
                   const std::string &err);

    ToolCall m_pending;              // 等待命令结束回填的调用
    std::string m_runningSession;    // 在跑的会话(取消时要按它停)
};

// ------------------------------------------------------------------
// terminal_open: 开一个会话(可选 cwd), 返回会话 id
// terminal_list: 列出会话(id / cwd / 是否在跑)
// terminal_close: 关掉一个会话(省得一直占着)
// ------------------------------------------------------------------
class JZTerminalOpenTool : public JZTerminalToolBase
{
public:
    explicit JZTerminalOpenTool(JZVirtualTerminal *term = nullptr);
    virtual void call(const ToolCall &params) override;
};

class JZTerminalListTool : public JZTerminalToolBase
{
public:
    explicit JZTerminalListTool(JZVirtualTerminal *term = nullptr);
    virtual void call(const ToolCall &params) override;
};

class JZTerminalCloseTool : public JZTerminalToolBase
{
public:
    explicit JZTerminalCloseTool(JZVirtualTerminal *term = nullptr);
    virtual void call(const ToolCall &params) override;
};

// ⚠ **文件编辑不在这里** —— 走 `JZCodeTools.h` 的 `JZModifyFileTool`(工具名 `modify_file`)。
//   理由: 它用 `ICodeToolHost` 那套区域判定, 而且成功时回改动位置的上下文、失败时回命中位置
//   (模型能立刻确认改对了地方 / 看出为什么没匹配上) —— 这是 shell 的 sed 给不了的。
//   终端这一层只管"跑命令", 不重复实现文件编辑。

#endif // !JZ_TERMINAL_TOOLS_H_
