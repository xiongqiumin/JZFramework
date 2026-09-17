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
//   - stream=true    : 执行中按 notifications/message 逐块推送日志(POST 内联 SSE), 结束推结果
//   - background=true : 后台运行, 立即返回 run_id(不流式), 日志用 terminal_query 取
//   - stream_filter / grep : 流式与查询时按流(stdout/stderr)或关键字过滤
class JZTerminalRunMcpTool : public JZMcpTool
{
public:
    explicit JZTerminalRunMcpTool(JZTerminalRegistry *reg = nullptr);

    void run(const Json::Value &id, const Json::Value &args) override;
    void cancel(const Json::Value &id) override;

private:
    void onRunDone(const Json::Value &id, bool ok, const std::string &out,
                   const std::string &err, const std::string &runId, JZVirtualTerminal *term);
    void emitChunk(const Json::Value &id, const std::string &stream, const std::string &chunk,
                   const std::string &streamFilter, const std::string &grep);

    JZTerminalRegistry *m_registry = nullptr;
    // 在跑的前台调用: call id -> 会话 id。每个 call id 对应一条前台命令, 用于 cancel
    // 按调用精确定位会话(前台命令落在不同会话时各自独立)。
    // 运行态本身在 JZVirtualTerminal 里按会话维护, 这里不复制那套状态。
    std::map<std::string, std::string> m_inflight;
};

// terminal_query: 取后台/历史运行的日志。
//   - run_id : 取指定运行(缺省时必须带 session, 取该会话最近一次运行)
//   - tail   : 只取最近 N 行(0=全部)
//   - stream / grep : 与 terminal_run 同义过滤
class JZTerminalQueryMcpTool : public JZMcpTool
{
public:
    explicit JZTerminalQueryMcpTool(JZTerminalRegistry *reg = nullptr);
    void run(const Json::Value &id, const Json::Value &args) override;

private:
    JZTerminalRegistry *m_registry = nullptr;
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
