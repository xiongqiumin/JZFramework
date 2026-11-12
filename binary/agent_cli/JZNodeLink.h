#ifndef JZ_NODE_LINK_H_
#define JZ_NODE_LINK_H_

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "JZAgentTool.h"
#include "JZNet/JZNetPacket.h"   // JZNetPackPtr(onPacket 的入参)
#include "JZObject.h"

class JZNetClient;
class JZNetPack;
class JZTimer;

// ---------------------------------------------------------------------------
// JZNodeLink —— cli 侧到一台 link 模式 agent_node 的连接
//
// 角色: node 是 server(JZNetServer), cli 是 client(JZNetClient) —— 方向固定,
//   node 必须能被 cli 连上, 但 node 自己不需要外网(见 docs/agent-hub-design.md §6.1.2)。
//
// 职责:
//   - 按配置连一台 node(带 token 握手), 断线按 1/2/4/…/30s 退避重连;
//   - 一次只允许一条 exec 在飞(设备上并跑只会互相拖死), 忙时直接拒绝;
//   - 超时双保险: 本地定时器到点发 execCancel 并本地收口, node 侧也有 timeoutMs 兜底。
//
// 多台 node 由 JZAgentCli 管理, 对模型只暴露一个 remote_ssh 工具(按 host 路由)。
// ---------------------------------------------------------------------------
class JZNodeLink : public JZObject
{
public:
    struct Config
    {
        std::string name;      // 节点名(= remote_ssh 的 host 参数)
        std::string host;      // node 地址
        int port = 7500;
        std::string token;     // 与 node 的 token 一致
    };

    struct ExecResult
    {
        std::string runId;         // 本次运行的 id(带节点前缀, 供 query/cancel 引用)
        bool ok = false;
        bool started = false;      // true = 后台已启动(结果还没定, 用 query 取)
        int exitCode = -1;
        bool timedOut = false;
        int64_t durationMs = 0;
        std::string cwd;
        std::string output;
        std::string error;
    };

    // terminal_query 的远端版本: 状态与日志(节点侧已按 tail/grep/incremental 过滤)
    struct QueryResult
    {
        std::string runId;         // 节点实际查的那条(省略 run_id 时由节点回填)
        bool found = false;
        bool running = false;
        bool ok = false;
        bool background = false;
        bool timedOut = false;
        int exitCode = -1;
        int64_t durationMs = 0;
        int64_t totalLines = 0;
        int64_t firstLine = 0;
        int64_t readPos = 0;
        std::string text;
        std::string error;
    };

    typedef std::function<void(const ExecResult &)> ExecCallback;
    typedef std::function<void(const QueryResult &)> QueryCallback;

    JZNodeLink(const Config &config, const std::string &clientId, JZObject *parent = nullptr);
    ~JZNodeLink() override;

    const Config &config() const { return m_config; }
    bool online() const { return m_online; }
    // 前台在飞或还有后台命令没结束都算忙(后台也算占用并发额度, 见节点侧 maxConcurrent)
    bool busy() const { return m_pending != nullptr || !m_bgRuns.empty(); }
    const std::string &lastError() const { return m_lastError; }
    const std::string &nodeName() const { return m_nodeName; }
    const std::string &rootDir() const { return m_rootDir; }
    const std::string &shell() const { return m_shell; }

    // 建立连接(异步; 失败只记状态, 由退避定时器继续重连)
    void connectToNode();

    // 跑一条命令。owner 标识发起它的任务队列, 取消时只停该队列的命令。
    // background=true: 节点立刻回 started(结果待查), 之后用 query()/cancelRun()。
    // session 空 = 用这个队列自己的终端(每个队列一台, cwd/export 互不干扰);
    // 非空 = 用指定会话(模型通过 remote_ssh action=open 建的那种)。
    // 返回 false = 没发起(离线 / 正忙), error 有原因。
    bool exec(const std::string &command, const std::string &cwd, int timeoutMs, bool background,
              const void *owner, const ExecCallback &cb, std::string &error,
              const std::string &session = std::string());

    // 会话管理: op = "open"(建/复用) / "close"(销毁, 会 kill 在跑的命令) / "list"。
    // 回包是异步的(节点一侧要过网络), 10s 没回按超时收口。
    using SessionCallback = std::function<void(bool ok, const std::string &text,
                                              const std::string &error)>;
    bool sessionOp(const std::string &op, const std::string &session, const std::string &cwd,
                   const SessionCallback &cb, std::string &error);

    // 按 runId 查一次状态与日志(等价本地 terminal_query; runId 是 exec 返回的那个)。
    // 同一时刻只允许一条查询在飞(查询是毫秒级的 RPC, 没必要排一队)。
    bool query(const std::string &runId, int tail, const std::string &grep, bool incremental,
               const QueryCallback &cb, std::string &error);

    // 停一条指定的运行(前台或后台)。返回 false = 没发出去(离线 / 已经结束)。
    bool cancelRun(const std::string &runId, std::string &error);

    // 取消指定队列当前在跑的命令(含后台); owner=nullptr 表示不限队列。
    void cancelRunning(const void *owner = nullptr);

jz_signals:
    // 上线/掉线/重连都发一次(管理台与日志用)
    JZSigVoid sigStateChanged;

protected:
    void onConnected();
    void onDisconnected();
    void onPacket(JZNetPackPtr pack);
    void finishPending(const ExecResult &result, bool notify);
    void finishQuery(const QueryResult &result, bool notify);
    void scheduleReconnect();
    // 保活: 5s 一次 ping。node 侧 20s 收不到就当作本端掉线 —— kill 在跑的命令 + 关连接,
    // 这正是 shell 的语义(终端没了, 前台任务跟着死), 不做断线续跑。
    void sendPing();

    Config m_config;
    std::string m_clientId;

    JZNetClient *m_client = nullptr;
    JZTimer *m_reconnectTimer = nullptr;
    JZTimer *m_pingTimer = nullptr;   // 保活(仅 online 时跑)
    int m_retryMs = 1000;         // 退避(1/2/4/…/30s)
    // 连接代际: 每次 onConnected 自增。hello 的 3s 超时/日志都要先比对它 ——
    // 否则旧连接的定时器可能在新连接刚建好时触发, 把新连接掐掉。
    int m_connGen = 0;

    bool m_online = false;
    bool m_helloAcked = false;
    std::string m_lastError;

    // node 自述(helloAck 带回来)
    std::string m_nodeName;
    std::string m_rootDir;
    std::string m_shell;

    struct Pending
    {
        std::string runId;
        std::string session;           // 落到节点上的哪个终端(每个任务队列一台)
        ExecCallback cb;
        JZTimer *timer = nullptr;
        int timeoutMs = 0;
        const void *owner = nullptr;   // 发起队列; 仅做身份比较, 不解引用
    };
    std::unique_ptr<Pending> m_pending;

    // 发起方(任务队列) -> 节点上的 session 名。每个队列一台独立终端: 各自的 cwd/export
    // 不会串到别的任务(以前的单终端是全局共享的 shell, 一个任务的 cd 会影响下一个)。
    std::string sessionFor(const void *owner);
    std::map<const void *, std::string> m_ownerSession;
    int m_sessionGen = 0;

    // 后台运行: runId -> 发起队列。留着是为了 busy() 与按 owner 取消(节点侧在跑)。
    std::map<std::string, const void *> m_bgRuns;

    struct QueryPending
    {
        std::string runId;
        QueryCallback cb;
        JZTimer *timer = nullptr;
    };
    std::unique_ptr<QueryPending> m_query;

    // 会话管理的在飞请求(open/close/list 都是一来一回, 同一时刻只允许一条)
    struct SessPending
    {
        std::string reqId;
        SessionCallback cb;
        JZTimer *timer = nullptr;
    };
    std::unique_ptr<SessPending> m_sess;
    void finishSession(bool ok, const std::string &text, const std::string &error);

    int m_runIdGen = 0;
    int m_sessReqGen = 0;
};

// ---------------------------------------------------------------------------
// JZRemoteSshTool —— 模型侧唯一的远端执行入口
//
//   remote_ssh(host, action?, session?, command?, cwd?, timeout_ms?) -> "exit=..时..\n<输出>"
//
// 多台 node 靠 host(= 节点名, 必填)路由, 所以模型工具表里永远只有一条。
// ---------------------------------------------------------------------------
class JZRemoteSshTool : public JZAgentTool
{
public:
    // resolver: host(节点名) -> 连接(找不到返回 nullptr);
    // nodeList: 写在工具描述里的节点清单(构造时生成, 运行期不变)
    JZRemoteSshTool(std::function<JZNodeLink *(const std::string &)> resolver,
                    const std::string &nodeList, const void *owner);

    void call(const ToolCall &params) override;

private:
    std::function<JZNodeLink *(const std::string &)> m_resolver;
    std::string m_nodeList;
    const void *m_owner = nullptr;   // 所属任务队列(固定身份, 不是每轮状态)
};

#endif // !JZ_NODE_LINK_H_
