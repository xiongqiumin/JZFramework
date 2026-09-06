#include "JZRuntimeDebugger.h"

#include "JZMcpServer.h"
#include "JZMcpCore.h"
#include "JZMcpTool.h"
#include "JZDebug.h"
#include "json/json.h"

// ----------------------------------------------------------------------------
// 调试原语接口定义（仅定义契约，不实现控制 JS 的逻辑）。
//
// 每个原语 = 一个独立 MCP 工具。这里用一张工具清单驱动注册：name/description/
// inputSchema 全部按接口规范写全；run() 统一回 reserved 占位响应，待后续接入
// 引擎（setEngine / setRuntime）后逐个补全实现。
//
// js_methods 为元工具，返回已注册工具名 + implemented 标记（纯元数据，不控制 JS）。
// ----------------------------------------------------------------------------

// 入参描述：name 字段名；type JSON schema 类型；required 是否必填；enums 枚举值（可选）。
// 注意：结构体作为聚合初始化用，不要加默认成员初始化器（否则在该工程 C++ 标准下
// 不再是聚合类型，ToolParam{...} 初始化会失败）。required 在构造时显式给出。
struct ToolParam
{
    std::string name;
    std::string type;
    bool required;
    std::vector<std::string> enums;
};

// 一个调试原语的静态定义（接口契约）。
struct DebugToolDef
{
    std::string name;
    std::string desc;
    std::vector<ToolParam> params;
};

// 所有调试原语清单（js_methods 为元工具，单独处理，不入此表）。
static const std::vector<DebugToolDef>& debugTools()
{
    static const std::vector<DebugToolDef> kTools = {
        // A. 求值 / 调用
        {"js_eval", "执行一段 JS 代码，阻塞至结果 settle（非 Promise 直接返回，Promise 等到 resolve/reject）。",
            std::vector<ToolParam>{ ToolParam{"code", "string", true}, ToolParam{"timeout", "integer", false} }},
        {"js_call", "调用一个 JS 函数（target 为函数句柄 int 或全局函数名 string），阻塞至结果 settle。",
            std::vector<ToolParam>{ ToolParam{"target", "string", true}, ToolParam{"args", "array", true}, ToolParam{"timeout", "integer", false} }},
        {"js_get_properties", "列举某句柄（对象/函数/数组）的属性，返回可渐进展开的键描述符。",
            std::vector<ToolParam>{ ToolParam{"handle", "integer", true}, ToolParam{"own", "boolean", false}, ToolParam{"enumerable", "boolean", false},
             ToolParam{"depth", "integer", false}, ToolParam{"maxKeys", "integer", false} }},
        {"js_globals", "列举全局作用域变量（等价于对 globalThis 调 js_get_properties）。",
            std::vector<ToolParam>{ ToolParam{"own", "boolean", false}, ToolParam{"enumerable", "boolean", false},
             ToolParam{"depth", "integer", false}, ToolParam{"maxKeys", "integer", false} }},

        // B. 内存
        {"js_memory", "返回 quickjs 内存使用概览（mallocCount / atomCount 等）。", std::vector<ToolParam>{}},
        {"js_gc", "触发 quickjs GC，返回回收前后内存统计。", std::vector<ToolParam>{}},

        // C. 运行时事件
        {"js_console_messages", "读取控制台消息环形缓冲（console.* 输出 + 注入的 logpoint 日志）。",
            std::vector<ToolParam>{ ToolParam{"since", "string", false}, ToolParam{"level", "string", false} }},
        {"js_exception_get", "读取最近一次未捕获异常（栈已翻译回 TS 源码位置）。", std::vector<ToolParam>{}},
        {"js_clear_logs", "清空控制台消息缓冲。", std::vector<ToolParam>{}},

        // D. UI 热重载
        {"js_ui_reload", "重跑工程 UI 构建入口并替换根控件子树（控件树检视复用 js_get_properties）。",
            std::vector<ToolParam>{ ToolParam{"moduleId", "string", false} }},

        // E. 执行控制 / 断点
        {"js_breakpoint_set", "设置断点/日志断点。side 默认 ts（按 TS 行，经 source map 翻 JS 行）；"
            "action 默认 pause（暂停），log 时为不暂停的日志断点（命中时在帧作用域求值 log 表达式并写入日志）。",
            std::vector<ToolParam>{ ToolParam{"side", "string", false, std::vector<std::string>{"ts", "js"}}, ToolParam{"url", "string", false}, ToolParam{"line", "integer", true},
             ToolParam{"action", "string", false, std::vector<std::string>{"pause", "log"}}, ToolParam{"log", "string", false}, ToolParam{"condition", "string", false} }},
        {"js_breakpoint_list", "列出所有断点（含 side / tsLine / jsLine / action / log / condition）。", std::vector<ToolParam>{}},
        {"js_breakpoint_remove", "按 id 移除断点。", std::vector<ToolParam>{ ToolParam{"id", "integer", true} }},
        {"js_pause", "协作式暂停（在字节码边界停住）。", std::vector<ToolParam>{}},
        {"js_resume", "从暂停态恢复执行。", std::vector<ToolParam>{}},
        {"js_step", "单步执行。mode: in 步入 / over 步过 / out 步出。",
            std::vector<ToolParam>{ ToolParam{"mode", "string", true, std::vector<std::string>{"in", "over", "out"}} }},
        {"js_stacktrace", "返回当前调用栈帧（funcName / url / line / column）。", std::vector<ToolParam>{}},
        {"js_scopes", "返回某栈帧作用域内的变量（name / type / preview / handle?），供 js_get_properties 展开。",
            std::vector<ToolParam>{ ToolParam{"frameIndex", "integer", true} }},
    };
    return kTools;
}

// 由入参清单构造 JSON schema。
static Json::Value buildInputSchema(const std::vector<ToolParam>& params)
{
    Json::Value schema(Json::objectValue);
    schema["type"] = "object";
    Json::Value props(Json::objectValue);
    Json::Value required(Json::arrayValue);
    for (const auto& p : params)
    {
        Json::Value f(Json::objectValue);
        f["type"] = p.type;
        if (!p.enums.empty())
        {
            Json::Value e(Json::arrayValue);
            for (const auto& v : p.enums)
                e.append(v);
            f["enum"] = e;
        }
        props[p.name] = f;
        if (p.required)
            required.append(p.name);
    }
    schema["properties"] = props;
    schema["required"] = required;
    return schema;
}

// 统一 reserved 响应。
static Json::Value reservedResult(const std::string& toolName)
{
    Json::Value result(Json::objectValue);
    Json::Value content(Json::arrayValue);
    Json::Value item(Json::objectValue);
    item["type"] = "text";
    item["text"] = toolName + " 接口已定义，尚未实现（reserved）";
    content.append(item);
    result["content"] = content;
    result["isError"] = false;
    return result;
}

// 通用 reserved 工具：按 DebugToolDef 配置 name/description/inputSchema，
// run() 统一回 reserved 占位（具体控制 JS 的实现后续接入引擎后补）。
class JZRuntimeReservedTool : public JZMcpTool
{
    JZ_OBJECT_DECLARE(JZRuntimeReservedTool)
public:
    explicit JZRuntimeReservedTool(JZObject* parent = nullptr) : JZMcpTool(parent) {}

    void configure(const DebugToolDef& def)
    {
        setName(def.name);
        setDescription(def.desc);
        setInputSchema(buildInputSchema(def.params));
        m_name = def.name;
    }

    void run(const Json::Value& id, const Json::Value& args) override
    {
        (void)args;
        sigFinished.emit(id, reservedResult(m_name));
    }

private:
    std::string m_name;
};
JZ_OBJECT_IMPL(JZRuntimeReservedTool, JZMcpTool)

// 元工具：自省已注册原语（纯元数据，不控制 JS）。
class JZRuntimeMethodsTool : public JZMcpTool
{
    JZ_OBJECT_DECLARE(JZRuntimeMethodsTool)
public:
    explicit JZRuntimeMethodsTool(JZObject* parent = nullptr) : JZMcpTool(parent)
    {
        setName("js_methods");
        setDescription("自省已注册的调试原语及其 implemented 标记（纯元数据）。");
        Json::Value schema(Json::objectValue);
        schema["type"] = "object";
        schema["properties"] = Json::Value(Json::objectValue);
        schema["required"] = Json::Value(Json::arrayValue);
        setInputSchema(schema);
    }

    void run(const Json::Value& id, const Json::Value& args) override
    {
        (void)args;
        Json::Value tools(Json::arrayValue);
        for (const auto& t : debugTools())
        {
            Json::Value e(Json::objectValue);
            e["name"] = t.name;
            e["implemented"] = false; // 当前均为 reserved
            tools.append(e);
        }

        Json::Value text(Json::objectValue);
        text["ok"] = true;
        text["tools"] = tools;

        Json::Value result(Json::objectValue);
        Json::Value content(Json::arrayValue);
        Json::Value item(Json::objectValue);
        item["type"] = "text";
        item["text"] = Json::writeString(Json::StreamWriterBuilder(), text);
        content.append(item);
        result["content"] = content;
        result["isError"] = false;
        sigFinished.emit(id, result);
    }
};
JZ_OBJECT_IMPL(JZRuntimeMethodsTool, JZMcpTool)

// ----------------------------------------------------------------------------
// JZRuntimeDebugger
// ----------------------------------------------------------------------------
JZ_OBJECT_IMPL(JZRuntimeDebugger, JZObject)

JZRuntimeDebugger::JZRuntimeDebugger(JZObject* parent)
    : JZObject(parent)
{
}

JZRuntimeDebugger::~JZRuntimeDebugger()
{
    stop();
}

bool JZRuntimeDebugger::start(int port)
{
    if (m_server)
        return true; // 已启动

    m_server = new JZMcpServer(this);
    m_server->core()->setServerInfo("JZRuntimeDebugger", "1.0.0");

    // 注册全部调试原语（接口已定义，run 回 reserved）。
    for (const auto& def : debugTools())
    {
        auto* tool = new JZRuntimeReservedTool(m_server->core());
        tool->configure(def);
        m_server->core()->registerTool(tool);
    }
    // 注册元工具 js_methods（自省）。
    m_server->core()->registerTool(new JZRuntimeMethodsTool(m_server->core()));

    if (!m_server->listenHttp(port))
    {
        jzError() << "JZRuntimeDebugger: HTTP 监听失败, port =" << port;
        delete m_server;
        m_server = nullptr;
        return false;
    }

    jzInfo().nospace() << "JZRuntimeDebugger: HTTP 监听 http://127.0.0.1:" + std::to_string(port) + "/mcp";
    return true;
}

void JZRuntimeDebugger::stop()
{
    if (m_server)
    {
        m_server->close();
        delete m_server;
        m_server = nullptr;
    }
}

bool JZRuntimeDebugger::evalJs(const std::string& code, std::string& result, std::string& error)
{
    // 预留接口：控制 JS 尚未实现
    (void)code;
    result.clear();
    error = "JS control not implemented yet (reserved interface)";
    return false;
}
