#ifndef JZ_TERMINAL_MCP_TOOLS_H_
#define JZ_TERMINAL_MCP_TOOLS_H_

// terminal 的 MCP 直出工具(不走 agent_tool 包装, 直接继承 JZMcpTool)。
// 与 agent_tool 的 JZTerminalTools 同为 JZVirtualTerminal 的两个门面之一:
//   - agent_tool 门面: 本地 code_agent 用(JZTerminalRunTool 等)
//   - mcp_tool  门面: 远程 MCP 客户端用(本文件), 支持流式日志(内联 SSE)

#include "JZMcpTool.h"
#include "json/json.h"

class JZVirtualTerminal;

// terminal_run: 跑一条命令。
//   - stream=true    : 执行中按 notifications/message 逐块推送日志(POST 内联 SSE), 结束推结果
//   - background=true : 后台运行, 立即返回 run_id(不流式), 日志用 terminal_query 取
//   - stream_filter / grep : 流式与查询时按流(stdout/stderr)或关键字过滤
class JZTerminalRunMcpTool : public JZMcpTool
{
public:
    explicit JZTerminalRunMcpTool(JZVirtualTerminal *term = nullptr);

    void run(const Json::Value &id, const Json::Value &args) override;
    void cancel(const Json::Value &id) override;

private:
    void onRunDone(const Json::Value &id, bool ok, const std::string &out,
                   const std::string &err, const std::string &runId, const std::string &session);
    void emitChunk(const Json::Value &id, const std::string &stream, const std::string &chunk);

    JZVirtualTerminal *m_term = nullptr;
    bool m_running = false;                 // 是否有前台(非后台)命令在跑
    std::string m_runningSession;
    std::string m_streamFilter = "all";      // "stdout" / "stderr" / "all"
    std::string m_grep;                      // 关键字过滤(空=不过滤)
};

// terminal_query: 取后台/历史运行的日志。
//   - run_id : 取指定运行(缺省时取该会话/全局最近一次运行)
//   - tail   : 只取最近 N 行(0=全部)
//   - stream / grep : 与 terminal_run 同义过滤
class JZTerminalQueryMcpTool : public JZMcpTool
{
public:
    explicit JZTerminalQueryMcpTool(JZVirtualTerminal *term = nullptr);
    void run(const Json::Value &id, const Json::Value &args) override;

private:
    JZVirtualTerminal *m_term = nullptr;
};

#endif // JZ_TERMINAL_MCP_TOOLS_H_
