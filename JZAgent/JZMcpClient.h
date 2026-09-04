#ifndef JZ_MCP_CLIENT_H_
#define JZ_MCP_CLIENT_H_

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "JZAgentTool.h"
#include "JZObject.h"
#include "json/json.h"

class JZNetworkAccessManager;
class JZNetworkReply;
class JZProcess;
class JZTimer;
class JZWebSocket;
class JZMcpClient;

/**
 * JZMcpClientTool —— 远端 MCP 工具在本地的代理。
 *
 * 对 JZAgent 来说它就是一个普通的 JZAgentTool: call() 把参数转成一次
 * MCP tools/call, 远端返回后再通过 sigResult 回传给 agent。
 *
 * 代理本身不保存任何远端信息(工具名映射、前缀规则都在 JZMcpClient 里),
 * 每次调用时按本地工具名去向 client 查对应的远端工具名。
 *
 * 生命周期归创建它的 JZMcpClient(以 client 为父对象); JZAgent 通过
 * JZAgent::addMcp() 只是登记这些指针, 不接管所有权。
 */
class JZMcpClientTool : public JZAgentTool
{
public:
    explicit JZMcpClientTool(JZMcpClient *client);

    virtual void call(const ToolCall &params) override;

private:
    void onCallDone(const ToolCall &call, bool ok, const Json::Value &value);

    JZMcpClient *m_client;
};

/**
 * JZMcpClient —— MCP 客户端。
 *
 * 职责:
 *   - 连接一个 MCP 服务器(stdio 子进程 / Streamable HTTP / WebSocket)
 *   - 拉取远端工具列表(tools/list), 为每个工具生成本地代理 JZMcpClientTool
 *   - 把代理工具的调用转成 tools/call 发给服务器, 结果回传给代理工具
 *
 * 协议版本 2026-07-28(与 JZMcpServer 一致, 无状态): 没有 initialize 握手,
 * 协议版本与客户端身份放在每个请求的 _meta 中, 服务器信息通过 server/discover 获取。
 * 需要握手的旧版服务器(2025-06-18 及更早, 带 Mcp-Session-Id 会话)不在支持范围内。
 *
 * 使用方式(MCP 客户端依赖事件循环, 需要先创建 JZApplication):
 *   JZMcpClient *client = new JZMcpClient();
 *   if (client->startProcess("python", {"server.py"}))
 *       agent->addMcp(client);       // 之后 client 归 agent 所有
 *
 * startProcess / startHttp / startWebSocket 都是阻塞的: 内部会跑事件循环,
 * 直到握手与工具列表拉取完成或超时。
 */
class JZMcpClient : public JZObject
{
    JZ_OBJECT_DECLARE(JZMcpClient)

public:
    // 一次 JSON-RPC 调用的回调: (是否成功, result 或 error 对象)
    using ReplyCallback = std::function<void(bool ok, const Json::Value &value)>;

    enum Transport { None, Process, Http, WebSocket };

    explicit JZMcpClient(JZObject *parent = nullptr);
    ~JZMcpClient();

    // ---- 建立连接(阻塞到握手与工具列表拉取完成) ----
    // stdio: 启动子进程, 通过 stdin/stdout 收发 JSON-RPC(一行一条)
    bool startProcess(const std::string &program, const std::vector<std::string> &arguments,
                      int timeoutMs = 10000);
    // Streamable HTTP: 每个请求一次 POST, 响应支持 JSON 与 SSE 两种格式
    bool startHttp(const std::string &url, int timeoutMs = 10000);
    bool startWebSocket(const std::string &url, int timeoutMs = 10000);

    void close();
    bool isReady() const;
    Transport transport() const;

    // 最近一次错误(连接失败、超时、传输中断等)
    const std::string &errorString() const;

    // ---- 远端工具对应的本地代理工具 ----
    const std::vector<JZAgentTool*> &tools() const;
    JZAgentTool *tool(const std::string &name) const;

    // 本地工具名前缀: 默认 "<serverName>__"(Claude 风格, 避免多个 server 之间重名),
    // 显式设置时以设置为准; 服务器名为空时退化为 "mcp__"。
    void setToolPrefix(const std::string &prefix);
    const std::string &toolPrefix() const;

    // 本地工具名 <-> 远端工具名 的映射(映射表由本客户端统一维护)
    std::string makeLocalName(const std::string &remoteName) const;
    std::string remoteToolName(const std::string &localName) const;

    // 单次 tools/call 的超时, 默认 120 秒
    void setCallTimeout(int ms);
    int callTimeout() const;

    std::string serverName() const;
    std::string serverVersion() const;

    // 一次 tools/call(供 JZMcpClientTool 使用)
    void callTool(const std::string &name, const Json::Value &args, ReplyCallback cb);

protected:
    struct Pending
    {
        ReplyCallback cb;
        JZTimer *timer = nullptr;   // 超时定时器, 属于本客户端
    };

    Json::Value makeRequest(const std::string &method, const Json::Value &params,
                            const std::string &protocolVersion);
    // protocolVersion 为空时表示使用 2026-07-28
    int sendRequest(const std::string &method, const Json::Value &params, ReplyCallback cb,
                    int timeoutMs, const std::string &protocolVersion = std::string());

    bool sendRaw(const std::string &text);
    void onRecvText(const std::string &text);
    void handleMessage(const Json::Value &msg);

    void finishPending(int id, bool ok, const Json::Value &value);
    void clearPending(bool notify);
    void setFailed(const std::string &error);

    // 发起一次请求并同步等待结果(内部跑事件循环)
    bool runSync(const std::string &method, const Json::Value &params, Json::Value &result,
                 int timeoutMs);
    // 连接前的准备: 关闭旧连接并清空上一次的错误
    bool prepareConnect();
    bool handshake(int timeoutMs);
    bool fetchTools(int timeoutMs);
    void pump(int ms);

    // 传输层事件
    void onProcessOutput();
    void onHttpReply(JZNetworkReply *reply);
    void onWsMessage(const std::string &text);

    Transport m_transport = None;
    JZProcess *m_process = nullptr;
    JZNetworkAccessManager *m_net = nullptr;
    JZWebSocket *m_ws = nullptr;
    std::string m_url;

    int m_nextId = 1;
    std::map<int, Pending> m_pending;
    std::string m_readBuffer;   // stdio 的行缓冲
    bool m_ready = false;
    std::string m_error;

    std::vector<JZAgentTool*> m_tools;
    std::map<std::string, std::string> m_remoteNames;   // 本地工具名 -> 远端工具名
    std::string m_toolPrefix;
    int m_callTimeoutMs = 120000;

    std::string m_serverName;
    std::string m_serverVersion;
    std::string m_protocolVersion;
};

#endif // !JZ_MCP_CLIENT_H_
