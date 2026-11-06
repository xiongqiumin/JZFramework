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
// 多台 node 由 JZAgentCli 管理(一个 map), 对模型只暴露一个 remote_ssh 工具。
// ---------------------------------------------------------------------------
class JZNodeLink : public JZObject
{
public:
    struct Config
    {
        std::string name;      // 对模型/日志的标识(remote_ssh 的 host 参数)
        std::string host;      // node 地址
        int port = 7500;
        std::string token;     // 与 node 的 token 一致
        bool isDefault = false;
    };

    struct ExecResult
    {
        bool ok = false;
        int exitCode = -1;
        bool timedOut = false;
        int64_t durationMs = 0;
        std::string cwd;
        std::string output;
        std::string error;
    };

    typedef std::function<void(const ExecResult &)> ExecCallback;

    JZNodeLink(const Config &config, const std::string &clientId, JZObject *parent = nullptr);
    ~JZNodeLink() override;

    const Config &config() const { return m_config; }
    bool online() const { return m_online; }
    bool busy() const { return m_pending != nullptr; }
    const std::string &lastError() const { return m_lastError; }
    const std::string &nodeName() const { return m_nodeName; }
    const std::string &rootDir() const { return m_rootDir; }
    const std::string &shell() const { return m_shell; }

    // 建立连接(异步; 失败只记状态, 由退避定时器继续重连)
    void connectToNode();

    // 跑一条命令。返回 false = 没发起(离线 / 正忙 / 已经有同名 run), error 有原因。
    bool exec(const std::string &command, const std::string &cwd, int timeoutMs,
              const ExecCallback &cb, std::string &error);

    // 取消当前在跑的命令(任务被重置时用)
    void cancelRunning();

jz_signals:
    // 上线/掉线/重连都发一次(管理台与日志用)
    JZSigVoid sigStateChanged;

protected:
    void onConnected();
    void onDisconnected();
    void onPacket(JZNetPackPtr pack);
    void finishPending(const ExecResult &result, bool notify);
    void scheduleReconnect();

    Config m_config;
    std::string m_clientId;

    JZNetClient *m_client = nullptr;
    JZTimer *m_reconnectTimer = nullptr;
    int m_retryMs = 1000;         // 退避(1/2/4/…/30s)

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
        ExecCallback cb;
        JZTimer *timer = nullptr;
        int timeoutMs = 0;
    };
    std::unique_ptr<Pending> m_pending;
    int m_runIdGen = 0;
};

// ---------------------------------------------------------------------------
// JZRemoteSshTool —— 模型侧唯一的远端执行入口
//
//   remote_ssh(host, command, cwd?, timeout_ms?) -> "exit=..时..\n<输出>"
//
// 多节点靠 host 路由, 所以模型工具表里永远只有一条; host 省略时用配置里标记
// default 的那台(只有一台 node 时就是它)。
// ---------------------------------------------------------------------------
class JZRemoteSshTool : public JZAgentTool
{
public:
    // resolver: host -> 连接(找不到返回 nullptr); defaultHost: 空 host 时用哪台;
    // nodeList: 写在工具描述里的节点清单(构造时生成, 运行期不变)
    JZRemoteSshTool(std::function<JZNodeLink *(const std::string &)> resolver,
                    std::function<std::string()> defaultHost,
                    const std::string &nodeList);

    void call(const ToolCall &params) override;

private:
    std::function<JZNodeLink *(const std::string &)> m_resolver;
    std::function<std::string()> m_defaultHost;
    std::string m_nodeList;
};

#endif // !JZ_NODE_LINK_H_
