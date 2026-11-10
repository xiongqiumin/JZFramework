#ifndef JZ_AGENT_NODE_PACKET_H_
#define JZ_AGENT_NODE_PACKET_H_

#include "JZNet/JZNetPacket.h"

// ⚠ 位置: 本协议只被 agent_cli 这一族的 exe(agent_cli / agent_node /
//   agent_cli_server / agent_cli_test)用, **不属于 JZFramework 的导出面** ——
//   所以它不在 JZAgent 模块里, 而是编进这些 exe 共用的静态库 agent_cli_packet
//   (见 binary/agent_cli/CMakeLists.txt)。因此这里的类和函数**都不带**
//   JZFRAMEWORK_API: 加了会变成 dllimport, 在 exe 里定义就报 C2491。
// ---------------------------------------------------------------------------
// agent_node(link 模式) <-> agent_cli 的执行面协议
//
// 设计见 docs/agent-hub-design.md §6.4:
//   - node 是 server(link 模式起 JZNetServer), cli 是 client(JZNetClient), 方向固定;
//   - link 模式**不起 MCP**, 所以这里只有一条"执行命令"的语义, 没有 tools/list;
//   - 多节点只在 cli 侧存在(一个 remote_ssh 工具按 host 路由), node 之间互不知情。
//
// 包类型从 NetPack_user 起: 1000/1001 给 node(这里), 1100+ 留给以后的 server<->cli 控制面。
// ⚠ 单包上限见 JZNetPacket.h 的 MG_MAX_PACK_SIZE(256KB): 输出必须两端都按上限裁,
//   否则大编译日志会把整条连接打成畸形包。
// ---------------------------------------------------------------------------
enum
{
    NodePack_hello = NetPack_user + 1,   // cli -> node: 上线自报(日志/审计用, 不做路由)
    NodePack_helloAck,                   // node -> cli: 确认 + 节点自述(name/rootDir/能力)
    NodePack_exec,                       // cli -> node: 跑一条命令
    NodePack_execResult,                 // node -> cli: 结果(输出/退出码/超时/后台已启动)
    NodePack_execCancel,                 // cli -> node: 停一条在跑的命令(任务被重置/超时时)
    NodePack_termQuery,                  // cli -> node: 按 runId 取状态/日志(等价 terminal_query)
    NodePack_termQueryResult,            // node -> cli: 上面那条的回应
};

// cli -> node: 上线
class JZNodePackHello : public JZNetPack
{
public:
    virtual int packetType() const;
    virtual void saveToStream(JZDataStream &s) const;
    virtual void loadFromStream(JZDataStream &s);

    std::string clientId;     // cli 的 agentName(日志用)
    std::string cliVersion;
};

// node -> cli: hello 的回应(顺带把节点的自述带回来, 供 /info 与管理台展示)
class JZNodePackHelloAck : public JZNetPack
{
public:
    JZNodePackHelloAck();

    virtual int packetType() const;
    virtual void saveToStream(JZDataStream &s) const;
    virtual void loadFromStream(JZDataStream &s);

    bool ok;
    std::string error;        // ok=false 时给原因(例如 link 模式没开终端)
    std::string nodeName;
    std::string version;
    std::string rootDir;      // 工作区(工具回 cwd 时也用这个)
    std::string shell;        // 实际探测到的 shell(提示词/排障用)
};

// cli -> node: 执行一条命令
class JZNodePackExec : public JZNetPack
{
public:
    JZNodePackExec();

    virtual int packetType() const;
    virtual void saveToStream(JZDataStream &s) const;
    virtual void loadFromStream(JZDataStream &s);

    std::string runId;        // cli 生成, 用于结果匹配与 cancel
    std::string command;      // 整条交给 shell(与 terminal_run 同一套语义)
    std::string cwd;          // 空 = 节点工作区根; 非空会拼成 `cd <cwd> && <command>`
    int timeoutMs = 120000;
    // 后台执行(等价 terminal_run background=true): 节点立刻回一条 started 结果,
    // 之后用 NodePack_termQuery 查日志/状态、NodePack_execCancel 停掉。
    // ⚠ 追加字段: 老版本按默认 false 读, 不影响以前的"跑完才回包"语义。
    bool background = false;
};

// node -> cli: 一次执行的结果
class JZNodePackExecResult : public JZNetPack
{
public:
    JZNodePackExecResult();

    virtual int packetType() const;
    virtual void saveToStream(JZDataStream &s) const;
    virtual void loadFromStream(JZDataStream &s);

    std::string runId;
    bool ok = false;          // 退出码为 0 且没超时
    int exitCode = -1;
    bool timedOut = false;
    int64_t durationMs = 0;
    std::string cwd;          // 实际执行时的 cwd(给模型对齐相对路径)
    std::string output;       // 合并的 stdout/stderr(已按包上限裁剪)
    std::string error;        // 被拒/起不来/超时/非零退出的原因
    // true = 后台运行已**启动**(不是最终结果): 输出/退出码还没定, 用 NodePack_termQuery 查。
    bool started = false;
};

// cli -> node: 按 runId 取一次运行的状态与日志(等价本地的 terminal_query)。
//   tail        : 只取过滤后的最近 N 行(<=0 = 全部)
//   grep        : 子串过滤(空 = 不过滤)
//   incremental : true = 只回上次查询之后新增的行(游标在节点侧推进, 轮询不重复灌上下文)
class JZNodePackTermQuery : public JZNetPack
{
public:
    JZNodePackTermQuery();

    virtual int packetType() const;
    virtual void saveToStream(JZDataStream &s) const;
    virtual void loadFromStream(JZDataStream &s);

    std::string runId;        // cli 的 runId(节点侧映射到终端自己的 runId)
    int tail = 0;
    std::string grep;
    bool incremental = false;
};

// node -> cli: 查询结果。状态字段与 ExecResult 同口径, 另带日志正文与行号信息。
class JZNodePackTermQueryResult : public JZNetPack
{
public:
    JZNodePackTermQueryResult();

    virtual int packetType() const;
    virtual void saveToStream(JZDataStream &s) const;
    virtual void loadFromStream(JZDataStream &s);

    std::string runId;
    bool found = false;       // false = 节点上已经没有这条运行记录(重启/淘汰/runId 拼错)
    bool running = false;
    bool ok = false;          // 已结束且退出码为 0
    bool background = false;
    bool timedOut = false;
    int exitCode = -1;
    int64_t durationMs = 0;
    int64_t totalLines = 0;   // 累计产出行数(只增)
    int64_t firstLine = 0;    // 本次 text 第一行的绝对行号
    int64_t readPos = 0;      // 查询后的新游标(仅 incremental 时有意义)
    std::string text;         // 已按 tail/grep/incremental 过滤好的日志
    std::string error;        // 收尾原因(超时/被杀/非零退出)
};

// cli -> node: 取消一次在跑的执行
class JZNodePackExecCancel : public JZNetPack
{
public:
    JZNodePackExecCancel();

    virtual int packetType() const;
    virtual void saveToStream(JZDataStream &s) const;
    virtual void loadFromStream(JZDataStream &s);

    std::string runId;
};

// 注册全部 node 包(幂等; 必须在 JZNetPackInit 之后调用, 两边进程各调一次)
void JZAgentNodePacketInit();

#endif // !JZ_AGENT_NODE_PACKET_H_
