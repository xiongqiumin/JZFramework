#ifndef JZ_RUNTIME_DEBUGGER_H_
#define JZ_RUNTIME_DEBUGGER_H_

#include <string>
#include "JZObject.h"

class JZMcpServer;
class JZJSEngine;     // 预留：控制 JS 用（求值/调用/对象检视的执行体）
class JZEngineRuntime; // 预留：日志/异常捕获源（js_console_messages / js_exception_get 需要）

// 运行时调试器。
// 基于 JZMcpServer 开启 HTTP 监听（MCP Streamable HTTP，端点 /mcp），供外部工具连接并"控制 JS"。
//
// 对外暴露的调试原语（每个 = 一个独立 MCP 工具，接口契约见下方清单；具体实现尚未完成，
// run() 统一回 reserved 占位响应）：
//   A 求值/调用 : js_eval, js_call, js_get_properties, js_globals
//   B 内存      : js_memory, js_gc
//   C 运行时事件 : js_console_messages, js_exception_get, js_clear_logs
//   D UI 热重载  : js_ui_reload
//   E 执行控制   : js_breakpoint_set, js_breakpoint_list, js_breakpoint_remove,
//                 js_pause, js_resume, js_step, js_stacktrace, js_scopes
//   元          : js_methods（自省已注册工具及 implemented 标记）
//
// 约定：所有工具响应为 JSON 对象；成功 {ok:true,...}，失败 {ok:false,error}，
//       未实现 {ok:false,reserved:true,error:"not implemented"}。
//       任意 JS 值统一归一化为值描述符 {handle?,type,preview,truncated?}。
class JZRuntimeDebugger : public JZObject
{
    JZ_OBJECT_DECLARE(JZRuntimeDebugger)
public:
    explicit JZRuntimeDebugger(JZObject* parent = nullptr);
    ~JZRuntimeDebugger();

    // 绑定被调试的 JS 引擎（预留接口：真正实现"控制 JS"前需先设置）。
    void setEngine(JZJSEngine* engine) { m_engine = engine; }

    // 绑定运行时（预留接口：console/异常捕获源；js_console_messages /
    // js_exception_get 实现前需先设置）。
    void setRuntime(JZEngineRuntime* rt) { m_runtime = rt; }

    // 启动 HTTP 监听（MCP Streamable HTTP，端点 /mcp）。port 默认 9222。
    // 成功返回 true（server 已 listen）。
    bool start(int port = 9222);
    void stop();

    // 预留接口：执行一段 JS 代码。暂未实现，返回 false 并置 error。
    bool evalJs(const std::string& code, std::string& result, std::string& error);

    JZMcpServer* server() const { return m_server; }

private:
    JZMcpServer*     m_server = nullptr;
    JZJSEngine*      m_engine = nullptr;  // 预留：控制 JS 用
    JZEngineRuntime* m_runtime = nullptr; // 预留：日志/异常源
};

#endif // !JZ_RUNTIME_DEBUGGER_H_
