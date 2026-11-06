#ifndef JZ_AGENT_CLI_SERVER_H_
#define JZ_AGENT_CLI_SERVER_H_

#include <map>
#include <string>
#include <vector>

#include "JZJson.h"
#include "JZNet/JZNetPacket.h"   // JZNetPackPtr(onHubPacket 的入参)
#include "JZObject.h"

class JZFrpServer;
class JZHttpServer;
class JZProcess;
class JZTimer;
class JZNetServer;
class JZHttpServerClient;
class JZHttpRequest;

// ---------------------------------------------------------------------------
// JZAgentCliServer —— agent_cli 管理服务器
//   1) (可选) 跑 frps 服务端, 与 agent_cli 的 frpc client 配对做内网穿透;
//   2) 管理一系列 JZAgentCli 实例: 每个作为一个子进程拉起并监管(启停/重启/状态);
//   3) 收集各 agent 的 stdout/stderr 日志, 提供后台网页 + 管理 API 查询。
//   网页/API 都在本类的 JZHttpServer 上; 端口由 config.http.* 控制。
// ---------------------------------------------------------------------------

// 后台网页 / 管理 API 配置
struct JZCliServerHttpConfig
{
    std::string bind = "127.0.0.1";
    int port = 8088;
    std::string token;          // 非空则要求 ?token= 或 Authorization: Bearer(防裸奔)
};

// 被管理的一个 agent_cli 实例的配置
struct JZCliServerAgentConfig
{
    std::string name;           // 唯一名(用于 API / 网页标识)
    bool enabled = true;        // 是否随 server 启动
    std::string cmd = "agent_cli";   // 程序; 默认解析到 server 同目录的 agent_cli(.exe)
    std::vector<std::string> args;   // 启动参数, 如 ["-c","agent1.json"]
    std::string dir;            // 工作目录(空 = server 同目录)
};

// 控制面(hub): cli 出站连进来的 JZNetServer
//   ⚠ 与 http.port(8088, 管理网页/API)是两个端口: 这是二进制协议, 只给 cli 用。
struct JZCliServerHubConfig
{
    std::string bind = "0.0.0.0";
    int port = 8090;
    std::string token;        // JZNet 握手口令; 空 = 不校验(仅限完全可信的网络)
};

// 已上线的一台 agent_cli(控制面连接)的视图
struct JZCliServerOnlineAgent
{
    uint64_t conn = 0;
    std::string agentId;
    std::string name;
    std::string version;
    std::string rootDir;
    std::string modelName;
    std::string nodes;         // 节点清单 JSON 数组字符串(cli 上报)
    bool busy = false;
    int round = 0;
    std::string taskId;
    int64_t lastSeen = 0;
    std::string lastEventType;
    std::string lastEventData;  // JSON(已按包上限裁过)
    bool acceptTasks = true;
};

// JZAgentCliServer 配置: frps 服务端 + 后台网页 + 被管理的一系列 agent_cli + 控制面
struct JZCliServerConfig
{
    // frps 服务端(可选; 与 agent_cli 的 frpc client 配对做内网穿透)
    bool frpEnabled = false;
    int frpBindPort = 7000;
    std::string frpToken;

    // 后台网页 / 管理 API
    JZCliServerHttpConfig http;

    // 控制面: 等 agent_cli 出站连进来(替代 v1 的"拉本机子进程")
    JZCliServerHubConfig hub;

    // 管理的一系列 agent_cli(各作为一个子进程拉起并监管)
    std::vector<JZCliServerAgentConfig> agents;
};

// 单个被管理 agent 的运行实例(实现细节, 见 cpp)
struct JZCliServerAgentInstance;

class JZAgentCliServer : public JZObject
{
public:
    explicit JZAgentCliServer(JZObject *parent = nullptr);
    ~JZAgentCliServer() override;

    bool init(const JZCliServerConfig &config, std::string &error);

    // 启动全部: frps(可选) + 后台网页/管理 API + 拉起所有 enabled 的 agent。事件循环前调用。
    void start();

    // ---- 对外的 agent 管理(网页/API 调用) ----
    bool spawnAgent(const std::string &name);   // 启动(已在跑则忽略)
    bool stopAgent(const std::string &name);    // 优雅停止
    bool restartAgent(const std::string &name); // 重启

    // 路由处理器(供本类内 handler 子类调用, 需 public)
    void onApiStatus(JZHttpServerClient *client, const JZHttpRequest &req);
    void onApiAgents(JZHttpServerClient *client, const JZHttpRequest &req);
    void onApiAgentLogs(JZHttpServerClient *client, const JZHttpRequest &req);
    void onApiAgentAction(JZHttpServerClient *client, const JZHttpRequest &req,
                          const std::string &action);  // start / stop / restart
    // 控制面: 提交/取消任务(body = POST /task 的字段 + agentId)
    void onApiTasks(JZHttpServerClient *client, const JZHttpRequest &req);
    void onApiTaskCancel(JZHttpServerClient *client, const JZHttpRequest &req);
    void onDashboard(JZHttpServerClient *client, const JZHttpRequest &req);

private:
    bool checkToken(const JZHttpRequest &req, std::string &err) const;
    void startFrp();
    void startHttp();
    void startHub();                     // 起控制面(JZNetServer)
    void onHubPacket(uint64_t conn, JZNetPackPtr pack);
    void onHubDisconnected(uint64_t conn);
    void failPendingTasksOf(const std::string &agentId, const std::string &error);
    Json::Value onlineAgentsJson() const;
    std::string resolveProgram(const JZCliServerAgentConfig &cfg) const;
    void appendLog(JZCliServerAgentInstance *inst, const std::string &chunk);
    // 管理台网页的落盘目录(<exeDir>/server_web); 页面 CSS/JS 也从这里读
    std::string dashboardDir() const;
    bool loadDashboardHtml(std::string &html, std::string &error) const;

    JZCliServerConfig m_config;
    JZFrpServer *m_frpServer = nullptr;
    JZHttpServer *m_http = nullptr;
    std::map<std::string, JZCliServerAgentInstance*> m_agents;  // name -> 实例
    int64_t m_startTime = 0;

    // ---- 控制面 ----
    struct PendingTask
    {
        std::string taskId;
        std::string agentId;
        JZHttpServerClient *client = nullptr;   // 提交方的连接(断开后不再回包)
        JZTimer *timer = nullptr;               // 任务超时(到点回 504 并请求 cli 取消)
        int64_t startMs = 0;
    };

    JZNetServer *m_hub = nullptr;
    std::map<uint64_t, JZCliServerOnlineAgent> m_onlineByConn;
    std::map<std::string, uint64_t> m_connByAgent;        // agentId -> conn
    std::map<std::string, PendingTask> m_pendingTasks;    // taskId -> 等待回包的提交方
    int m_taskSeq = 0;
};

#endif // !JZ_AGENT_CLI_SERVER_H_
