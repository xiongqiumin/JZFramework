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
class JZTimer;
class JZNetworkReply;
class JZProcess;
class JZTimer;
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
 *   - 连接一个 MCP 服务器(stdio 子进程 / Streamable HTTP)
 *   - 拉取远端工具列表(tools/list), 为每个工具生成本地代理 JZMcpClientTool
 *   - 把代理工具的调用转成 tools/call 发给服务器, 结果回传给代理工具
 *
 * 协议版本 2026-07-28(与 JZMcpServer 一致, 无状态): 没有 initialize 握手,
 * 协议版本与客户端身份放在每个请求的 _meta 中, 服务器信息通过 server/discover 获取。
 * 需要握手的旧版服务器(2025-06-18 及更早, 带 Mcp-Session-Id 会话)不在支持范围内。
 *
 * 使用方式一(信号驱动, 推荐):
 *   JZMcpClient *client = new JZMcpClient();
 *   connect(&client->sigFinished, [client]() {
 *       if (client->isReady())
 *           agent->addMcp(client);   // 连接成功, 之后 client 归 agent 所有
 *       else
 *           ...;                     // 失败, errorString() 有原因
 *   });
 *   client->startProcess("python", {"server.py"});
 *
 * 使用方式二(同步式等待, 不想接信号时):
 *   JZMcpClient client;
 *   client.startHttp("http://127.0.0.1:3900/mcp");
 *   client.waitForFinished();        // 阻塞直到本次连接流程结束
 *   if (client.isReady()) { ... }    // 成功, tools() 可用
 *
 * startProcess / startHttp 都是异步的: 只负责发起并立即返回
 * (返回值表示"是否成功发起"); 连接、server/discover 与 tools/list 全部结束(无论
 * 成功或失败)后发一次 sigFinished。结束后用 isReady() 判断结果, 失败原因见
 * errorString()。请先 connect 好 sigFinished 再调用 startXxx。
 */
class JZMcpClient : public JZObject
{
    JZ_OBJECT_DECLARE(JZMcpClient)
    friend class JZMcpClientTool;

public:
    // 一次 JSON-RPC 调用的回调: (是否成功, result 或 error 对象)
    using ReplyCallback = std::function<void(bool ok, const Json::Value &value)>;

    enum Transport { None, Process, Http };

    explicit JZMcpClient(JZObject *parent = nullptr);
    ~JZMcpClient();

    // ---- 建立连接(异步) ----
    // 三个 startXxx 都只负责发起并立即返回(true=成功发起, 例如子进程已启动 /
    // HTTP 客户端已就绪 / WS 连接请求已发出)。连接流程(server/discover + tools/list)
    // 结束时会发 sigFinished: 成功则 isReady()==true、tools() 可用; 失败原因在
    // errorString()。调用前请先 connect 好 sigFinished, 或用 waitForFinished()
    // 同步等待结果。
    // stdio: 启动子进程, 通过 stdin/stdout 收发 JSON-RPC(一行一条)
    bool startProcess(const std::string &program, const std::vector<std::string> &arguments,
                      int timeoutMs = 10000);
    // Streamable HTTP: 每个请求一次 POST, 响应支持 JSON 与 SSE 两种格式
    bool startHttp(const std::string &url, int timeoutMs = 10000);

    void close();
    // 阻塞直到本次连接流程结束(等同 sigFinished 已发出)。
    // 供"发起后同步等结果"的场景使用; 结束后用 isReady() / errorString() 判断结果。
    // 依赖事件循环驱动(JZApplication), 应在主线程调用;
    // 不要在 sigFinished 回调或其它事件派发栈内调用(会死锁)。
    void waitForFinished();
    bool isReady() const;
    Transport transport() const;

jz_signals:
    // 一次连接流程结束: server/discover + tools/list 成功(isReady()==true)或
    // 任一步失败/超时/进程退出/中途断开。结果用 isReady() / errorString() 判断。
    JZSigVoid sigFinished;

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

    // 传输层事件
    void onProcessOutput();
    void onHttpReply(JZNetworkReply *reply);

    void finishPending(int id, bool ok, const Json::Value &value);
    void clearPending(bool notify);
    void setFailed(const std::string &error);

    // 连接前的准备: 关闭旧连接并清空上一次的错误
    bool prepareConnect();
    // 异步建立连接: server/discover -> tools/list, 结束(成功或失败)发 sigFinished
    void beginNegotiate();
    void onDiscoverDone(bool ok, const Json::Value &value);
    void onToolsDone(bool ok, const Json::Value &value);
    void abortConnect(const std::string &error);   // 连接阶段失败统一收口

    Transport m_transport = None;
    JZProcess *m_process = nullptr;
    JZNetworkAccessManager *m_net = nullptr;
    std::string m_url;

    int m_nextId = 1;
    std::map<int, Pending> m_pending;
    std::string m_readBuffer;   // stdio 的行缓冲
    bool m_ready = false;       // 当前是否可用(连接 + 工具列表已就绪)
    bool m_finished = false;    // 本次连接流程是否已结束(成功或失败), waitForFinished 用
    std::string m_error;

    std::vector<JZAgentTool*> m_tools;
    std::map<std::string, std::string> m_remoteNames;   // 本地工具名 -> 远端工具名
    std::string m_toolPrefix;
    int m_callTimeoutMs = 120000;
    int m_timeoutMs = 10000;    // 本次连接各步请求(server/discover、tools/list)的超时

    std::string m_serverName;
    std::string m_serverVersion;
    std::string m_protocolVersion;
};

#endif // !JZ_MCP_CLIENT_H_
