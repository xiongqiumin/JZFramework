#ifndef JZ_TERMINAL_MCP_TOOLS_H_
#define JZ_TERMINAL_MCP_TOOLS_H_

// terminal 的 MCP 直出工具(不走 agent_tool 包装, 直接继承 JZMcpTool)。
//
// 每次 run 从池里**借一台**终端, 跑完归还 —— 所以多条调用可以真并发,
// 一个调用也永远独占一台终端(不会和别人的命令抢同一个执行槽)。

#include <map>
#include <string>

#include "JZMcpTool.h"
#include "json/json.h"

class JZVirtualTerminal;
class JZTerminalPool;

// terminal_run: 跑一条命令。
// ⭐ 单前台任务指的是**一台终端**一次只跑一条; 并发靠池里多台终端。
//   没有 run_id / 后台模式 / 查询工具 —— 回包就是这次运行的输出。
class JZTerminalRunMcpTool : public JZMcpTool
{
public:
    explicit JZTerminalRunMcpTool(JZTerminalPool *pool = nullptr);

    void run(const Json::Value &id, const Json::Value &args) override;
    void cancel(const Json::Value &id) override;

private:
    void onRunDone(const Json::Value &id, bool ok, const std::string &out,
                   const std::string &err);

    JZTerminalPool *m_pool = nullptr;
    // 在跑的调用: call id -> 借到的那台终端(取消时精确停它, 不误伤别人的)
    std::map<std::string, JZVirtualTerminal *> m_inflight;
};

// ⛔ terminal_open / terminal_list / terminal_close 已删除(2026-11-12):
//   执行改成无状态之后, 这三个工具做不了任何事情(open 改变不了环境, list 只回一条,
//   close 会把正在用的终端连根拔掉) —— 模型却会先 open 再 run, 白绕一步。

#endif // !JZ_TERMINAL_MCP_TOOLS_H_
