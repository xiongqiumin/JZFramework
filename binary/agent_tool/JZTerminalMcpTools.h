#ifndef JZ_TERMINAL_MCP_TOOLS_H_
#define JZ_TERMINAL_MCP_TOOLS_H_

// terminal 的 MCP 直出工具(不走 agent_tool 包装, 直接继承 JZMcpTool)。
// 通过 JZTerminalRegistry 拿到"本次调用对应的 JZVirtualTerminal"——
// 工具只依赖注册表接口, 不与具体终端绑定(满足"不要和 JZVirtualTerminal 绑定")。
// 多会话 / 生命周期由 agent_node 用 map<session, JZVirtualTerminal> 管。

#include "JZMcpTool.h"
#include "JZTerminalRegistry.h"
#include "json/json.h"

class JZVirtualTerminal;

// terminal_run: 跑一条命令。
// ⭐ 单前台任务: 一次只有一条命令, 没有 run_id / 后台模式 / 查询工具 ——
//   回包就是这次运行的输出; 要后台就在命令里写 `&`(输出跟命令自己的重定向走)。
class JZTerminalRunMcpTool : public JZMcpTool
{
public:
    explicit JZTerminalRunMcpTool(JZTerminalRegistry *reg = nullptr);

    void run(const Json::Value &id, const Json::Value &args) override;
    void cancel(const Json::Value &id) override;

private:
    void onRunDone(const Json::Value &id, bool ok, const std::string &out,
                   const std::string &err, JZVirtualTerminal *term);

    JZTerminalRegistry *m_registry = nullptr;
    // 在跑的前台调用: call id -> 会话 id(取消时按会话找到终端)
    std::map<std::string, std::string> m_inflight;
};

// terminal_open: 开一个会话(一个独立终端实例, 各自 cwd), 返回会话 id
class JZTerminalOpenMcpTool : public JZMcpTool
{
public:
    explicit JZTerminalOpenMcpTool(JZTerminalRegistry *reg = nullptr);
    void run(const Json::Value &id, const Json::Value &args) override;

private:
    JZTerminalRegistry *m_registry = nullptr;
};

// terminal_list: 列出会话(id / cwd / 是否正在跑)
class JZTerminalListMcpTool : public JZMcpTool
{
public:
    explicit JZTerminalListMcpTool(JZTerminalRegistry *reg = nullptr);
    void run(const Json::Value &id, const Json::Value &args) override;

private:
    JZTerminalRegistry *m_registry = nullptr;
};

// terminal_close: 关掉一个会话(对应的终端被销毁, 在跑命令会被终止)
class JZTerminalCloseMcpTool : public JZMcpTool
{
public:
    explicit JZTerminalCloseMcpTool(JZTerminalRegistry *reg = nullptr);
    void run(const Json::Value &id, const Json::Value &args) override;

private:
    JZTerminalRegistry *m_registry = nullptr;
};

#endif // JZ_TERMINAL_MCP_TOOLS_H_
