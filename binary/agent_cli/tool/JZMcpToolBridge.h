#ifndef JZ_MCP_TOOL_BRIDGE_H_
#define JZ_MCP_TOOL_BRIDGE_H_

#include <map>
#include <string>

#include "JZAgentTool.h"
#include "JZMcpTool.h"
#include "json/json.h"

// ---------------------------------------------------------------------------
// JZMcpToolBridge —— 把任意 JZAgentTool 当作一个 MCP 工具挂出去
//
// 为什么需要它: 工具实现只有一份(agent_tool 里全是 JZAgentTool 派生), 而"谁来托管
// 这些工具"有两套 ——
//   本地 code_agent: JZAgent 直接消费 JZAgentTool, 天然就位;
//   远端 agent_node: JZMcpServer 只认 JZMcpTool。
// 本类就是这两者之间的转换器。
//
// 两边形状的差异与对应关系:
//   元数据: ToolDefine{name, description, vector<ParamSchema>, required_parameters}
//        ->  name / description / inputSchema(JSON Schema, 见 jzToolDefineToMcpSchema)。
//            这个方向**无损**: ToolDefine 是工具作者声明的事实来源, toJson() 只是把它写出来。
//            (反方向 JSON Schema -> ToolDefine 才有损 —— ParamSchema::fromJson 解析 items
//             时只取 type, 嵌套 properties 会丢。那是 JZMcpClientTool 那侧的问题。)
//   入参:   (Json::Value id, Json::Value args)
//        ->  ToolCall{id, function_name, params, function_args}
//   出参:   sigResult(ToolCallResult{id, isOk, result:string})
//        ->  sigFinished(id, {content:[{type:"text",text}], isError})
//   取消:   JZMcpTool::cancel(请求 id)
//        ->  反查本地 id -> JZAgentTool::cancel(本地 id) + sigCanceled(请求 id)
//
// 所有权: 构造即**接管** inner —— inner 必须没有 parent, 随本对象一起销毁。
// 用法: core->registerTool(new JZMcpToolBridge(new JZTerminalRunTool(term)));
// (registerTool 会把 bridge 挂到 core 下, 所以 bridge 自己不用带 parent。)
// ---------------------------------------------------------------------------

// ToolDefine -> MCP 的 (name, description, inputSchema)。
// ToolDefine::toJson() 产出的是 OpenAI 的 function 对象
//   { name, description, parameters:{type:"object", properties:{...}, required:[...]} }
// 而 MCP 要的 inputSchema 就是那个 parameters, 正好一一对应。
// name 为空返回 false。
bool jzToolDefineToMcpSchema(const ToolDefine &def, std::string &name,
                             std::string &description, Json::Value &inputSchema);

class JZMcpToolBridge : public JZMcpTool
{
public:
    explicit JZMcpToolBridge(JZAgentTool *inner);
    ~JZMcpToolBridge() override;

    JZAgentTool *inner() const { return m_inner; }

    // ---- MCP 侧入口 ----
    void run(const Json::Value &id, const Json::Value &args) override;
    void cancel(const Json::Value &id) override;

protected:
    void onInnerResult(const ToolCallResult &result);

private:
    JZAgentTool *m_inner = nullptr;

    // 本地调用 id(给 inner 用, 字符串) -> MCP 请求 id(JSON-RPC, 可能是数字)
    std::map<std::string, Json::Value> m_pending;
    int m_idGen = 0;
};

#endif // !JZ_MCP_TOOL_BRIDGE_H_
