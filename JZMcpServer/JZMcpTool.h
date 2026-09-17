#ifndef JZ_MCP_TOOL_H_
#define JZ_MCP_TOOL_H_

#include <string>
#include "JZObject.h"
#include "json/json.h"

/**
 * JZMcpTool —— MCP 工具基类(取代原来的 ToolHandler 函数对象)。
 *
 * 每个工具是一个对象: 携带元数据(name/description/inputSchema),
 * 通过重写 run() 启动执行。run() 不应通过返回值给出结果, 而应在执行结束时
 * 发射以下信号之一, 由 JZMcpCore 捕获并回调客户端:
 *   - sigFinished(id, result) : 执行完成(成功或错误结果), 携带请求 id 与 MCP 结果对象(content + isError)
 *   - sigCanceled(id)         : 被取消(携带请求 id, 对应 MCP notifications/cancelled)
 *
 * 取消支持:
 *   - JZMcpCore 收到 notifications/cancelled 时会调用本工具的 cancel(id);
 *   - 基类 cancel() 默认仅发射 sigCanceled(id); 子类应重写 cancel() 以维护自身的
 *     取消状态(例如置一个自定义标志), 并在 run() 中周期性检查该状态, 一旦取消应尽快退出;
 *   - 核心依据 sigCanceled(id) 回送取消响应。
 */
class JZMcpTool : public JZObject
{
    JZ_OBJECT_DECLARE(JZMcpTool)

public:
    explicit JZMcpTool(JZObject* parent = nullptr);
    ~JZMcpTool();

    // ---- 元数据 ----
    void setName(const std::string& name) { m_name = name; }
    void setDescription(const std::string& desc) { m_description = desc; }
    void setInputSchema(const Json::Value& schema) { m_inputSchema = schema; }

    const std::string& name() const { return m_name; }
    const std::string& description() const { return m_description; }
    const Json::Value& inputSchema() const { return m_inputSchema; }

    // ---- 执行: 子类重写 ----
    // 启动执行; 结果通过 sigFinished / sigCanceled 回送, 不靠返回值。
    //   id  : 本次请求的 JSON-RPC 请求 id(与 cancel 对应)
    //   args: 工具调用参数
    virtual void run(const Json::Value& id, const Json::Value& args);

    // ---- 取消(虚函数, 取消逻辑由子类实现) ----
    //   id: 要取消的请求 id(由 JZMcpCore 收到 notifications/cancelled 时传入)
    // 基类默认实现仅发射 sigCanceled(id); 子类应重写本函数维护自身取消状态,
    // 并在 run() 中检查该状态, 一旦取消应尽快退出并(通常)调用 sigCanceled.emit(id)。
    virtual void cancel(const Json::Value& id);

jz_signals:
    // 执行完成(成功或错误结果): (请求 id, MCP 结果对象)
    JZSignal<const Json::Value&, const Json::Value&> sigFinished;
    // 被取消: (请求 id)
    JZSignal<const Json::Value&> sigCanceled;
    // 流式通知(工具执行过程中主动推送, 如 terminal_run 的逐块日志):
    //   (请求 id, 通知对象) —— 通知对象应是完整 JSON-RPC notification(如 notifications/message)。
    //   核心据此在 POST 响应内联 SSE 分块回送, 不结束请求; 最终仍以 sigFinished 结束。
    JZSignal<const Json::Value&, const Json::Value&> sigNotify;

protected:
    std::string m_name;
    std::string m_description;
    Json::Value m_inputSchema;
};

#endif // JZ_MCP_TOOL_H_
