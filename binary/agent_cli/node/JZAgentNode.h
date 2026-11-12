#ifndef JZ_AGENT_NODE_H_
#define JZ_AGENT_NODE_H_

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "JZAgentNodePacket.h"   // link 模式: 与 agent_cli 的执行面协议
#include "JZCodeTools.h"     // ICodeToolHost / JZVirtualTerminalConfig / TrustedDir
#include "JZObject.h"
#include "JZTerminalRegistry.h"   // JZTerminalRegistry: 会话 -> 终端 的注册表(本节点实现)

class JZMcpServer;
class JZNetServer;
class JZAgentTool;
class JZMcpTool;           // 直接挂进 MCP 的工具(不经过 agent_tool 包装)
class JZVirtualTerminal;   // 执行/审核面, 见 agent_tool/JZVirtualTerminal.h
class JZTimer;

// ---------------------------------------------------------------------------
// 节点配置: main.cpp 从 config.json 读取后交给 JZAgentNode::init()
//   (默认读取 exe 同目录的 config.json, 可用 -c 指定)
// ---------------------------------------------------------------------------
class JZNodeConfig
{
public:
    // 节点唯一名。它会成为 MCP 的 serverInfo.name, 而本地 JZMcpClient 默认拿
    // "<serverName>__" 当工具名前缀 —— 所以**多节点时这个名字必须唯一**, 否则
    // 后上线的节点工具会被 JZAgent::addMcp() 当成重名静默丢掉(只打一条 warning)。
    std::string nodeName = "agent_node";
    std::string version = "1.0.0";

    // HTTP 监听(端点 /mcp)。host 默认 0.0.0.0(所有网卡)。
    // ⚠ 节点**没有鉴权**: 谁连上来谁就能用这台机器的终端。只想让本机的 agent 连就写
    // 127.0.0.1; 跨机器访问请走 ssh 隧道之类的通道, 别直接把 39000 暴露出去。
    std::string host = "0.0.0.0";
    int port = 39000;

    // ---- 运行模式(见 docs/agent-hub-design.md §6.1.2) ----
    //   "mcp"(默认) = 单独当被控端 MCP server(行为与以前完全一致);
    //   "link"      = 配合 agent_cli: **不起 MCP**, 只起 JZNetServer 供 cli 的 remote_ssh 调用。
    // ⚠ 两种模式 node 都是 server 角色, cli 永远是 client —— 所以 node 必须能被 cli 连上。
    std::string mode = "mcp";
    // link 模式的握手口令(JZNetServer::setToken)。空 = 拒绝启动 link 模式(裸奔的执行口子)。
    std::string token;
    // link 模式: 每条 cli 连接同时允许几条命令在跑(每条终端本身仍是串行的)。
    // 设备上建议保持 1: 并跑几条编译只会互相拖死。<=0 表示不限。
    int maxConcurrent = 1;

    // 文件工具的根目录(相对路径按它拼接), 同时也是区域模型里的"工作区"(可读写)。
    // 为空时取进程当前工作目录。
    std::string rootDir;

    // 授信目录(编译器/SDK 之类), 默认只读, 需要写就设 writable。
    std::vector<TrustedDir> trustedDirs;

    // ---- 工具开关 ----
    // 终端是这台机器上唯一的执行面: 关掉它, 文件操作(ls/cat/cp/grep...)也没了 ——
    // 那些都是 shell 命令。
    bool allowTerminal = true;
    // 文件工具(read_file / write_file / modify_file)。关掉后只剩终端,
    // 读写就得走 shell(大文件分段读、改代码校验唯一匹配这些能力会一起没掉)。
    bool allowFileTools = true;

    // ---- 权限档位(三选一, 见 JZVirtualTerminalConfig.h) ----
    //   strict   : 只跑登记过的命令 + 区外只读 + 禁联网 + 禁装依赖
    //   standard : 未登记命令放行 + 区外只读 + 允许联网 + 禁装依赖   ← 默认
    //   loose    : standard + 允许装/卸依赖 + 授信目录可写
    // ⚠ 安全底线(提权/改系统设置/关机/格式化/删工作区外)三档都拦, 没有开关。
    // ⚠ 节点是 HTTP 零鉴权的: 对外暴露时用 strict, 别用 loose。
    PermissionProfile profile = PermissionProfile::Standard;
    // 档位之上的单项覆盖(配置里写了才算, has* = 是否显式配置)
    bool allowPackageManager = false;
    bool allowNetwork = true;
    bool allowUnknownPrograms = true;
    bool hasAllowPackageManager = false;
    bool hasAllowNetwork = false;
    bool hasAllowUnknownPrograms = false;

    // 跑命令用哪个 shell。空 = 自动探测: **优先 bash**(POSIX 先看 /bin、/usr/bin 再扫 PATH;
    // Windows 找 Git for Windows 的 Git/bin/bash.exe), 找不到回退 /bin/sh;
    // 两个都没有会直接报错, 不静默退回 cmd / 别的解释器。
    std::string shellProgram;

    // ---- 会话(终端) ----
    // 同时最多开几个会话/终端: 每个会话是一个独立终端(各自 cwd / 在跑的进程 / 运行记录)。
    //   mcp  模式: 模型忘了 terminal_close 就会一路堆上去, 靠这个上限 + 空闲回收兜住;
    //   link 模式: 每条 cli 连接最多几台终端(session), 连接断开时全部销毁。
    // <=0 表示不限。
    int maxSessions = 8;
    // 会话空闲多久回收(秒), <=0 表示不回收。默认 15 分钟。
    int recycleSec = 15 * 60;

    // 日志落文件(空 = 只打控制台)。长驻进程看日志全靠它。
    std::string logFile;
};

// ---------------------------------------------------------------------------
// JZAgentNodeHost —— 节点侧的"工具宿主"
//
// 实现 ICodeToolHost(组合, 不继承), 给 agent_tool 里的通用工具提供:
// 区域配置、命令策略、读文件。节点这边没有 LRU 缓存, 直读。
//
// 注意: 现在真正的执行面是 JZVirtualTerminal(命令整条交给 shell)。这个宿主只负责
// 把节点配置(工作区/授信目录/策略)喂给它 —— 区域的约束能力也随之变成"命令级":
// 重定向目标写不了区外、edit_file 写不了区外, 但**程序自己**做什么管不到
// (`rm -rf ~/x` 拦不住)。真正的边界只能是沙箱。
// ---------------------------------------------------------------------------
class JZAgentNodeHost : public ICodeToolHost
{
public:
    explicit JZAgentNodeHost(const JZNodeConfig *config);

    // 从 config 重建区域视图(init 之后调一次; 配置可热改时再调)
    void refresh();

    const JZVirtualTerminalConfig &toolTerminalConfig() const override { return m_terminalConfig; }
    bool toolReadFile(const std::string &absPath, std::string &text, std::string &error) override;

private:
    const JZNodeConfig *m_config = nullptr;
    JZVirtualTerminalConfig m_terminalConfig;
};

// ---------------------------------------------------------------------------
// JZAgentNode —— 被控端节点: 把本机能力包成一个 MCP server
//
// 传输方式(HTTP)由 main.cpp 决定, 本类只负责"这台机器上有什么工具":
//   终端(MCP 直出门面):  terminal_run / terminal_open / terminal_list / terminal_close
//   文件:  read_file / write_file / modify_file
// 文件操作(ls/cat/mkdir/rm/cp/mv/grep...)不单独做工具 —— 它们都是 shell 命令,
// 由 terminal_run 覆盖; 只有 modify_file 留在我们这边(sed 不校验唯一匹配)。
// 终端 MCP 工具直接继承 JZMcpTool; 文件工具经 JZMcpToolBridge 挂成 MCP。
// 两组各有开关(allowTerminal / allowFileTools), 关掉哪个, instructions 里就只写
// 剩下的那部分(见 buildInstructions)。
//
// 本类还管会话的生命周期, 但两种模式不同:
//   mcp  模式: open() 建(受 maxSessions 限制)、close() 销毁、后台定时器按 recycleSec 回收空闲的
//              (MCP over HTTP 是无状态的, 拿不到"客户端断开"这个信号, 只能靠空闲兜底);
//   link 模式: 终端按「连接 + session」懒创建, 连接断(或被保活判死)就全部销毁 ——
//              生命周期跟着连接走, 没有空闲回收。
//  JZVirtualTerminal 自己不管这些。
// ---------------------------------------------------------------------------
class JZAgentNode : public JZObject, public JZTerminalRegistry
{
public:
    explicit JZAgentNode(JZObject *parent = nullptr);
    ~JZAgentNode() override;

    // 建 server、注册工具、填 serverInfo。必须在 listen 之前调用。
    void init(const JZNodeConfig &config);

    // link 模式: 只建"一台终端 + 执行口子", 不建 JZMcpServer、不注册 MCP 工具。
    // (配合 agent_cli 时用; 见 JZNodeConfig::mode)
    void initLink(const JZNodeConfig &config);

    // link 模式: 起 JZNetServer 等 cli 连进来(token 为空时拒绝启动)。
    bool startLinkServer(std::string &error);
    bool linkListening() const;

    JZMcpServer *server() const { return m_server; }
    const JZNodeConfig &config() const { return m_config; }
    JZTerminalRegistry *terminalRegistry() { return this; }
    int sessionCount() const { return (int)m_terminals.size(); }

    // ---- JZTerminalRegistry: 会话 -> 终端 ----
    JZVirtualTerminal *terminal(const std::string &sessionId, std::string &error) override;
    std::string open(const std::string &cwd, std::string &error) override;
    bool close(const std::string &sessionId) override;
    std::vector<JZVirtualTerminal::SessionInfo> list() override;

protected:
    // 把 agent_tool 的一个工具经 bridge 挂进 MCP(接管 tool 所有权)
    void addTool(JZAgentTool *tool);
    // 把 JZMcpTool 直接挂进 MCP(接管 tool 所有权)—— 如 terminal 的 MCP 门面
    void addTool(JZMcpTool *tool);

    std::string buildInstructions() const;

    // ---- link 模式 ----
    // 把配置里的路径统一成 "/" 分隔的绝对路径(区域判定是纯字符串前缀比较)
    void normalizeConfig(JZNodeConfig &config);
    // 一条 exec 请求: 审核由终端做, 这里只负责换 cwd 与回包
    // background=true: 立刻回一条 started, 结束再补一条"结束"通知(不带 output)
    void runLinkExec(uint64_t conn, const std::string &session, const std::string &runId,
                     const std::string &command, const std::string &cwd, int timeoutMs,
                     bool background);
    void onLinkPacket(uint64_t conn, JZNetPackPtr pack);
    void sendLinkResult(uint64_t conn, const JZNodePackExecResult &result);

    // 一条连接名下的终端: session 名("" = 默认) -> 终端。每条终端同一时刻只跑一条命令,
    // 所以在跑的 runId 也记在这里(cancel 归属判定: 只停 runId 对得上的那条)。
    struct LinkSession
    {
        JZVirtualTerminal *term = nullptr;
        std::string runId;        // 这台终端当前在跑的 runId(空闲为空)
    };
    struct LinkClient
    {
        std::map<std::string, LinkSession> sessions;
        int64_t lastPingMs = 0;   // 0 = 这条连接还没发过 ping(老 cli, 不做超时判定)
    };
    // 终端生命周期 = 连接生命周期: 连接断开(或被判死)就全部销毁, 不做空闲回收。
    std::map<uint64_t, LinkClient> m_linkClients;

    // 取(必要时懒建)某条连接的某台终端。超过 maxSessions 时返回 nullptr + error。
    JZVirtualTerminal *linkTerm(uint64_t conn, const std::string &session, std::string &error);
    // 连接断开: 销毁它名下全部终端(析构会 kill 在跑的进程)
    void dropLinkClient(uint64_t conn);
    // 该连接当前有几条命令在跑(用于 maxConcurrent)
    int linkRunningCount(const LinkClient &client) const;

    // 保活: cli 每 5s 发一次 ping。socket 半开(拔网线/断电)时不会报断开,
    // 只有靠"多久没收到包"来发现 —— 超时按掉线处理(kill 在跑的命令 + 关连接)。
    void startLinkWatchdog();
    void checkLinkAlive();

    JZNetServer *m_linkServer = nullptr;         // link 模式的 server 角色
    JZTimer *m_linkWatchdog = nullptr;
    int m_linkSessionGen = 0;                    // open 没给名字时自动编号(s1/s2...)

    // 空闲回收: 起一个周期定时器扫会话(recycleSec <= 0 时不开)。
    // ⚠ JZVirtualTerminal 自己不管生命周期(它只把 recycleMs 存着给上层看),
    //   不扫的话"空闲回收"这个配置就是一句空话 —— 会话会一直堆到进程退出。
    void startRecycleTimer();
    // 关掉空闲超过 recycleSec 的会话(在跑命令的不动)
    void recycleIdleSessions();

    JZNodeConfig m_config;
    JZAgentNodeHost m_host;
    JZMcpServer *m_server = nullptr;

    // 会话 -> 终端。每个会话一个独立 JZVirtualTerminal(各自 cwd / 前后台进程 / 运行记录);
    // 生命周期(创建/销毁/空闲回收)由节点管。终端是 this 的子对象(随节点销毁),
    // MCP 工具只持注册表、不绑定具体终端。
    std::map<std::string, JZVirtualTerminal *> m_terminals;
    int m_sessionIdGen = 0;
    JZTimer *m_recycleTimer = nullptr;
};

#endif // !JZ_AGENT_NODE_H_
