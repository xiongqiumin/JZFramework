#ifndef JZ_MCP_CORE_H_
#define JZ_MCP_CORE_H_

#include <string>
#include <vector>
#include <map>
#include <set>
#include <functional>
#include "JZObject.h"
#include "json/json.h"

class JZMcpTool;

/**
 * JZMcpCore —— MCP 协议核心。
 *
 * 职责:
 *   - JSON-RPC 2.0 的解析 / 序列化(从 JSON 文本输入到 JSON 文本输出)
 *   - 工具注册与管理(registerTool / tools/list): 工具以 JZMcpTool 对象形式注册
 *   - 工具调用: 通过 runToolAsync() 提交执行, 连接 JZMcpTool 的完成信号
 *     (sigFinished / sigCanceled, 均携带请求 id), 工具发射信号时回调 ProcessCallback
 *     把响应回送给对端
 *   - 取消: 收到 notifications/cancelled 时, 通过 cancelRequest() 调用对应工具的 cancel()
 *
 * 协议版本: 2026-07-28(最新, 无状态, 不做兼容)。
 *   - 无 initialize 握手; 协议版本与客户端能力由每请求/每响应的 _meta
 *     (io.modelcontextprotocol/protocolVersion 等)携带。
 *   - 服务器信息通过 server/discover 暴露, 并在每个 result 的 _meta 中附带
 *     io.modelcontextprotocol/serverInfo。
 *   - 所有 result 带 resultType("complete" / "input_required")。
 *
 * JZMcpCore 不关心传输方式(HTTP / WebSocket / 纯文本)。
 * 它由 JZMcpServer(后端)持有, 后端负责把输入喂给 process()、并把
 * process() 产生的输出通过回调发回对端。

 */
class JZMcpCore : public JZObject
{
public:
    /**
     * 已注册的工具记录。
     */
    struct Tool
    {
        int toolId;        // 注册时分配的唯一序号
        JZMcpTool* tool;   // 工具对象(生命周期由本核心接管)
    };

    explicit JZMcpCore(JZObject* parent = nullptr);
    ~JZMcpCore();

    // 设置服务器信息(出现在 server/discover 响应与 result 的 _meta.serverInfo 中)
    void setServerInfo(const std::string& name, const std::string& version);

    // 设置可选的服务器说明(出现在 server/discover 响应 instructions 字段, 2026-07-28)
    void setInstructions(const std::string& instructions) { m_instructions = instructions; }

    /**
     * 注册一个工具对象。
     * 对象生命周期由本核心接管(以本核心为父), 调用方无需手动 delete。
     * @return 该工具被分配的 toolId; 传入 nullptr 时返回 -1。
     */
    int registerTool(JZMcpTool* tool);

    /**
     * 取消某个正在执行的请求(对应 MCP notifications/cancelled)。
     * 会找到该请求对应的 JZMcpTool 并调用其 cancel()。
     * @param requestId 客户端在 notifications/cancelled 中携带的 requestId
     */
    void cancelRequest(const Json::Value& requestId);

    /**
     * 异步处理一段 JSON-RPC 文本(单请求或批量数组)。
     * 结果通过 cb 回送, 不阻塞调用线程(便于后端在传输线程上异步处理)。
     *   cb 签名: void(bool hasResponse, const std::string& outJson)
     *     - hasResponse: 是否有响应体(整段为通知时为 false, 此时 outJson 为空)
     *     - outJson:     响应文本(JSON-RPC 响应或解析错误)
     * @return true 表示已提交处理(结果稍后由 cb 异步回送)。
     */
    using ProcessCallback = std::function<void(bool hasResponse, const std::string& outJson)>;
    bool process(const std::string& body, ProcessCallback cb);

    // ---- 供 JZMcpServer 后端查询 ----
    const std::string& serverName() const { return m_serverName; }
    const std::string& serverVersion() const { return m_serverVersion; }

private:
    /**
     * 在途请求: 保存对应工具、请求 id 与回送回调, 以便信号触发时回送并清理。
     */
    struct PendingRequest
    {
        JZMcpTool* tool = nullptr;
        Json::Value id;          // 请求 id(用于构造响应)
        ProcessCallback cb;
    };

    // 单请求分发 -> 响应 Json::Value(异步方法通过 asyncHandled 标记已由信号异步回送)
    Json::Value dispatch(const Json::Value& request, bool& isNotification,
                         ProcessCallback cb, bool& asyncHandled);

    // _meta 命名空间常量(io.modelcontextprotocol/*)
    static const char* kMetaProtocolVersion;
    static const char* kMetaClientCapabilities;
    static const char* kMetaServerInfo;
    static const char* kMetaClientInfo;

    // 校验请求 _meta 中的协议版本: 不一致返回 UnsupportedProtocolVersionError(-32022),
    // 否则返回 null。versionMismatch 为 true 时表示缺少 _meta 也视为不兼容(严格模式)。
    Json::Value checkProtocolVersion(const Json::Value& request) const;

    // server/discover: 替代旧版 initialize 握手, 暴露版本/能力/身份
    Json::Value onDiscover() const;

    Json::Value onToolsList();
    void onToolsCall(const Json::Value& id, const Json::Value& params,
                     ProcessCallback cb, bool& asyncHandled);
    void onCancel(const Json::Value& params);

    // 提交工具执行: 连接完成信号并调用 run(); 信号触发后通过 cb 回送响应
    void runToolAsync(int toolId, const Json::Value& id, const Json::Value& args, ProcessCallback cb);

    // 每个工具只连接一次完成信号(JZSignal 的断开接口有缺陷, 故不重复连接/断开)
    void ensureConnected(JZMcpTool* tool);
    // 工具完成信号回调: canceled=true 表示来自 sigCanceled
    void onToolSignal(JZMcpTool* tool, const Json::Value& id, const Json::Value& result, bool canceled);

    const Tool* findTool(const std::string& name) const;
    const Tool* findTool(int toolId) const;

    // JSON-RPC 响应/错误构造(纯 Json::Value)
    static Json::Value makeResult(const Json::Value& id, const Json::Value& result);
    static Json::Value makeError(const Json::Value& id, int code, const std::string& message);
    static Json::Value makeErrorData(const Json::Value& id, int code,
                                     const std::string& message, const Json::Value& data);
    // 给 result 补充 _meta.serverInfo(2026-07-28 要求每响应附带服务器身份)
    Json::Value withServerMeta(const Json::Value& result) const;

    // JSON 文本 <-> Json::Value
    static bool parseJson(const std::string& text, Json::Value& out);
    static std::string writeJson(const Json::Value& value);

    // 请求 id -> 字符串键(用于登记在途请求)
    static std::string idKey(const Json::Value& id);

    std::string        m_serverName;
    std::string        m_serverVersion;
    std::string        m_protocolVersion;
    std::string        m_instructions;      // 可选, initialize 响应的 instructions 字段
    std::vector<Tool>  m_tools;
    int                m_nextToolId;    // 自增分配 toolId
    std::map<std::string, PendingRequest> m_running;        // requestId 键 -> 在途请求
    std::set<JZMcpTool*> m_toolConnected;                   // 已连接完成信号的工具
};

#endif // JZ_MCP_CORE_H_
