#ifndef JZ_AGENT_HUB_LINK_H_
#define JZ_AGENT_HUB_LINK_H_

#include <functional>
#include <string>

#include "JZJson.h"
#include "JZNet/JZNetPacket.h"   // JZNetPackPtr(onPacket 的入参)
#include "JZObject.h"

class JZNetClient;
class JZTimer;

// ---------------------------------------------------------------------------
// JZHubLink —— cli 侧到 agent_cli_server 的控制面连接(managed 模式)
//
// 角色: server 是 server(JZNetServer), cli 是 client(JZNetClient)。
//   - 出站连接 ⇒ 内网机器不用 frp、不用端口映射, server 也不要知道 cli 的地址;
//   - 握手 token 只派生加密密钥(JZNet 不比对内容), 所以**必须**靠 hello/helloAck
//     的 3s 超时给出"token 或端口不对"的明确错误;
//   - 断线按 1/2/4/…/30s 退避重连, 重连后重新 hello(server 侧重建视图)。
//
// 任务上行/下行:
//   server -> cli: taskSubmit / taskCancel / reset
//   cli -> server: taskEvent / taskEnd / heartbeat
// 事件与回包的 JSON 与 SSE / POST /task 的取值完全一致(见 JZAgentHubPacket.h)。
// ---------------------------------------------------------------------------
class JZHubLink : public JZObject
{
public:
    struct Config
    {
        std::string agentId;      // server 侧登记的身份(空则用 agentName)
        std::string name;         // 展示名
        std::string version;
        std::string rootDir;
        std::string project;      // 项目名(一个 cli 一个 project)
        std::string modelName;
        std::string host;         // server 地址
        int port = 8090;
        std::string token;
    };

    // server 下发任务的入口(taskId 由 server 生成, 回包时要带回)
    typedef std::function<void(const std::string &taskId, const Json::Value &body)> SubmitHandler;
    typedef std::function<void(const std::string &taskId)> TaskIdHandler;

    // 远端文件操作(管理台的文件树/目录选择器): server 只给 op + JSON 参数,
    // 由 cli 在**自己这台机器**上执行 —— 所以文件树看到的是目标 agent 的文件系统。
    struct FsResult
    {
        bool ok = false;
        std::string error;
        std::string body;        // list/create/rename/delete: JSON; read: 原始字节
        int64_t size = 0;        // read: 文件总大小
        int64_t mtime = 0;
        bool truncated = false;  // read: 只给了前一段(包上限)
    };
    typedef std::function<FsResult(const std::string &op, const std::string &body)> FsHandler;
    void setFsHandler(FsHandler h) { m_onFs = std::move(h); }

    explicit JZHubLink(const Config &config, JZObject *parent = nullptr);
    ~JZHubLink() override;

    const Config &config() const { return m_config; }
    bool online() const { return m_online; }
    const std::string &lastError() const { return m_lastError; }
    int64_t lastAckServerTimeMs() const { return m_serverTimeMs; }

    // 节点清单(JSON 数组字符串)。server 靠它显示"这台 cli 挂了哪些 node";
    // 每次 hello/心跳都重新取一次, 所以节点上下线不用额外包。
    void setNodesProvider(std::function<std::string()> provider) { m_nodesProvider = std::move(provider); }

    void setTaskSubmitHandler(SubmitHandler h) { m_onSubmit = std::move(h); }
    void setTaskCancelHandler(TaskIdHandler h) { m_onCancel = std::move(h); }
    void setResetHandler(std::function<void(const std::string &scope)> h) { m_onReset = std::move(h); }
    void setStateHandler(std::function<void()> h) { m_onState = std::move(h); }

    void connectToServer();

    // 任务状态变化时调一次(心跳定时器会带着它上报)
    void setTaskState(bool busy, int round, const std::string &taskId);

    void sendTaskEvent(const std::string &taskId, const std::string &type, const Json::Value &data);
    void sendTaskEnd(const std::string &taskId, const Json::Value &result);

protected:
    void onConnected();
    void onDisconnected();
    void onPacket(JZNetPackPtr pack);
    void sendHello();
    void sendHeartbeat();
    void scheduleReconnect();
    std::string nodesJson() const;
    void notifyState();

    Config m_config;
    std::string m_agentId;      // 生效的 agentId(配置空则回落 agentName)

    JZNetClient *m_client = nullptr;
    JZTimer *m_heartbeat = nullptr;
    int m_retryMs = 1000;

    bool m_online = false;
    bool m_helloAcked = false;
    bool m_acceptTasks = true;
    std::string m_lastError;
    int64_t m_serverTimeMs = 0;

    bool m_busy = false;
    int m_round = 0;
    std::string m_taskId;

    std::function<std::string()> m_nodesProvider;
    SubmitHandler m_onSubmit;
    TaskIdHandler m_onCancel;
    FsHandler m_onFs;
    std::function<void(const std::string &)> m_onReset;
    std::function<void()> m_onState;
};

#endif // !JZ_AGENT_HUB_LINK_H_
