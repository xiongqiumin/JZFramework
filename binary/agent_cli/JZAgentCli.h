#ifndef JZ_AGENT_CLI_H_
#define JZ_AGENT_CLI_H_

#include <atomic>
#include <functional>
#include <string>
#include <map>
#include <vector>

#include "JZObject.h"
#include "JZAgent.h"          // JZAgent / JZAgentConfig / ToolCall / ToolCallResult
#include "JZCodeTools.h"      // ICodeToolHost / JZVirtualTerminalConfig / TrustedDir
#include "JZVirtualTerminal.h"

class JZAgentTool;
class JZVirtualTerminal;
class JZNetworkAccessManager;
class JZHttpServer;
class JZHttpServerClient;
class JZHttpRequest;
class JZProcess;
class JZFrpClient;
class JZMcpClient;
class JZNodeLink;
class JZHubLink;

// ---------------------------------------------------------------------------
// FRP 内网穿透配置(client 角色 = frpc)
//   ⚠ JZAgentCli 只承担"客户端"角色(frpc): 连到远端 frps, 把本机服务(如本机 HTTP
//     任务接口)经代理暴露到远端 frps 的 remotePort 上。
//   "服务端"角色(frps)由单独的 JZAgentCliServer(见 server/ 目录)承担。
//   默认关; 需要时在 config.json 的 frp.client.* 打开。
// ---------------------------------------------------------------------------
struct JZCliFrpProxy
{
    std::string name;        // 代理名(远端 frps 侧标识)
    std::string localIp;     // 本地服务地址(默认 127.0.0.1)
    int localPort = 0;       // 本地服务端口
    int remotePort = 0;      // 在 frps 上注册的对外端口
};

struct JZCliFrpClientConfig   // frpc: 本端连到远端 frps, 暴露本地服务
{
    bool enabled = false;
    std::string serverAddr;   // 远端 frps 地址
    int serverPort = 7000;    // 远端 frps 端口
    std::string token;
    std::vector<JZCliFrpProxy> proxies;
};

struct JZCliFrpConfig
{
    JZCliFrpClientConfig client;   // 客户端(frpc); 服务端(frps)在 JZAgentCliServer
};

// ---------------------------------------------------------------------------
// MCP 客户端配置: 连一个外部 MCP 服务器, 把它的远端工具挂进本 agent 的工具表。
//
//   "动态增加" = **只改配置(不重编译/不改代码)** —— 在 config.json 的 mcp.servers
//   里加减一条, 重启进程即可; 启动时逐个异步连接, 连上才登记工具, 连不上只记日志。
//
//   ⚠ 三条必须知道的约束:
//     ① 连接时机: JZAgent 只允许在 Inactive(**首个任务之前**)改工具表与系统提示词
//        (改工具表会破坏会话的 KV 前缀缓存, 见 JZAgent::rejectWhileActive), 所以
//        MCP 是"启动时挂一次"的: 任务已经开跑之后再连上也挂不进去(只记警告);
//     ② 执行位置: 远端工具在**对端**执行, 不经过本机 terminal 的区域/路径审核
//        (那套只约束本地工具), 等于把对端的能力引进来 —— 所以有总开关 mcp.enabled;
//     ③ 命名: 本地工具名默认 "<serverName>__<工具名>"(serverName 由对端 serverInfo
//        给), 与本地工具重名的会被跳过(JZAgent::addMcp 只打 warning)。
// ---------------------------------------------------------------------------
struct JZCliMcpServer
{
    std::string name;                 // 标识(日志用); 也作为没配 toolPrefix 时的提示名
    std::string transport = "stdio";  // stdio(起子进程, stdin/stdout 一行一条 JSON-RPC) / http(Streamable HTTP)
    std::string command;              // stdio: 可执行文件(按 PATH 查找)
    std::vector<std::string> args;    // stdio: 命令行参数
    std::string url;                  // http: MCP 端点, 如 http://127.0.0.1:39000/mcp
    std::string toolPrefix;           // 工具名前缀; 空 = 用对端 serverName 的 "<serverName>__"
    int timeoutMs = 10000;            // 连接(server/discover + tools/list)超时(ms)
    int callTimeoutMs = 120000;       // 单次 tools/call 超时(ms)
};

// ---------------------------------------------------------------------------
// 一台 link 模式 agent_node 的连接配置(见 docs/agent-hub-design.md §6)
//   ⚠ 列表是**静态**的: 启动时按配置连好, 运行期不增删(连不上就标离线)。
//   多台 node 在模型侧只体现为一个 remote_ssh 工具的 host 参数。
// ---------------------------------------------------------------------------
struct JZCliNodeConfig
{
    std::string name;          // 对模型/日志的标识(remote_ssh 的 host)
    std::string host;          // node 地址
    int port = 7500;
    std::string token;         // 与 node 的 token 一致
    bool isDefault = false;    // host 省略时用这台(只有一个 node 时自动是它)
};

// ---------------------------------------------------------------------------
// 控制面: 连 agent_cli_server(managed 模式, 见 docs/agent-hub-design.md §3)
//   host 非空即 managed —— cli **出站**连 server, 所以内网机器不用 frp / 不用端口映射。
//   managed 与 standalone **互斥**: managed 时不起 HTTP、不起 frp(配了也忽略)。
// ---------------------------------------------------------------------------
struct JZCliServerLinkConfig
{
    std::string host;       // server 地址; 空 = standalone(不起控制面)
    int port = 8090;        // server 的 hub 端口(不是管理网页那个 8088)
    std::string token;      // 与 server 的 hub.token 一致(JZNet 握手口令)
    std::string agentId;    // server 侧登记的身份; 空 = 用 agentName
};

// ---------------------------------------------------------------------------
// agent_cli 配置
//   与 agent_node 类似, 但多了 LLM 连接信息(modelUrl/Name/Token) —— 因为 agent_cli
//   自己就跑一个 JZAgent(直接驱动本地工具), 而不是把工具暴露给远端 agent 调。
// ---------------------------------------------------------------------------
struct JZCliConfig
{
    std::string agentName = "agent_cli";
    std::string version = "1.0.0";

    // ---- LLM ----
    std::string modelUrl;
    std::string modelName;
    std::string modelToken;
    int netTimeout = 120 * 1000;   // 网络请求超时(ms)
    int maxRounds = 100;           // 单任务最大对话轮次(-1=不限)
    int maxToken = 160000;         // 单任务 token 预算上限(上下文, 见 JZAgent::windowTokens())

    // 请求体附加字段(llm.body): 各家私有参数 / 采样参数, 逐字段并进请求体, 同名以它为准。
    // 治"输出复读"最有效的是服务端采样侧, 例如:
    //   {"repeat_penalty": 1.05, "dry_multiplier": 0.8, "dry_base": 1.75, "dry_allowed_length": 2}
    Json::Value extraBody;
    // 出错重试: 本地跑重试几乎没有成本, 所以预算给得比较宽 —— 瞬时错误(连接失败/超时/5xx)
    // 发条"继续"就能接着干, 不该让整个任务判失败。
    int maxRetries = 10;           // 瞬时错误后"接着发继续任务"的次数, 0=不重试
    int maxFixRetries = 2;         // 可修复错误(请求被拒/上下文超长)后"让模型改一版"的次数
    int retryDelayMs = 1000;       // 两次重试之间的等待(ms)

    // ---- 循环检测(命中直接中止本轮, 报 JZAgent::LoopError) ----
    //   两类"原地打转":
    //     ① 工具反复用**相同参数**调同一个工具(每一步都成功 ⇒ 不报错), 只把上下文
    //        一轮轮堆大(每轮重发整段 prompt), 一路烧到 maxRounds;
    //     ② 思考/回复文本尾部原地打转(同一行刷屏、同一段反复、周期串复读),
    //        输出能涨到上万 token 还不结束。
    //   结果也相同 = 证据更强(再试必然还是这个结果), 阈值更低; 结果不同可能只是
    //   合法轮询(等编译/等运行)。
    //   中止后在 agent_cli 里按"换做法"重试(见 classifyError)。
    bool loopDetect = true;
    int loopWindow = 24;             // 工具: 只看最近 N 步工具调用(0=不限窗口)
    int loopRepeat = 5;              // 工具: 同签名重复 N 次 -> 中止
    int loopSameResult = 5;          // 工具: 同签名且结果也相同 N 次 -> 中止
    // 轮询类工具(terminal_query / terminal_list): 同参数反复调用是正常用法(查后台任务进度),
    // 用上面的小阈值会误杀 ⇒ 单独放宽(见 JZLoopConfig::pollRepeat / pollSameResult)。
    int loopPollRepeat = 16;         // 轮询类工具: 同签名重复 N 次 -> 中止
    int loopPollSameResult = 6;      // 轮询类工具: 同签名且结果也相同 N 次 -> 中止
    int loopLineRepeat = 6;          // 输出: 同一行连续重复 N 次 -> 中止
    int loopBlockRepeat = 3;         // 输出: 同一段(空行分隔)连续重复 N 次 -> 中止
    int loopPeriodMax = 256;         // 输出: 周期串最长多少字符
    int loopPeriodRepeat = 8;        // 输出: 周期串至少重复多少次

    // ---- 验证脚本门控循环的默认值(/task 的 max_verify_rounds / verify_timeout 可覆盖) ----
    int verifyMaxRounds = 30;      // 验证失败最多喂回 agent 几轮
    int verifyTimeoutMs = 60000;   // 单次验证脚本运行上限(ms)

    // ---- 长期记忆(跨任务: 开工注入 + memory 工具读写) ----
    //   每次任务开始时把记忆注入任务消息, 任务过程中模型用 memory 工具 append 结论。
    //   注入放在任务消息里而不是 system prompt —— system prompt 是进程级的, 改它会连带
    //   把 KV 前缀缓存整段作废; 任务消息本来就每次都是新的。
    bool memoryEnabled = true;
    std::string memoryFile;        // 记忆文件; 空 = <rootDir>/.agent_memory.md (相对路径按 rootDir 解析)
    int memoryMaxBytes = 8192;     // 注入/读取上限(超出只给最近的; 追加的硬上限是它的 4 倍)

    // ---- 宿主常驻说明(拼进 system prompt 一次) ----
    //   典型用途: 玩法任务书 / 项目约定这类**整个会话都该在场**的约束。它只进 system prompt 一次,
    //   之后每轮 /task 只叠加新消息 —— 不用把同一段介绍每轮重复贴一遍, 也不靠"模型还记得"。
    //   extraFile 的相对路径按 rootDir 解析; 两个都给时 = 文件内容 + 内联文本(内联在后)。
    std::string systemExtra;
    std::string systemExtraFile;

    // ---- 工作区 / 工具 ----
    std::string rootDir;
    std::vector<TrustedDir> trustedDirs;

    // ---- 远端执行节点(link 模式 agent_node) ----
    //   空 = 纯本地执行(工具表里不出现 remote_ssh)。
    //   非空 = 注册一个 remote_ssh 工具, 按 host 路由到对应 node。
    std::vector<JZCliNodeConfig> nodes;

    bool allowTerminal = true;
    bool allowFileTools = true;
    bool allowPackageManager = false;
    bool allowNetwork = false;
    bool unrestricted = false;

    // ---- 网络工具(web_search / web_fetch) ----
    //   与上面的 allowNetwork 是两码事: 那个管"shell 命令能不能联网(下载)", 这个管
    //   "给模型挂不挂联网工具"。agent_cli 是自己在本地跑, 所以默认开。
    bool allowNetworkTools = true;
    std::string webSearchUrl;      // 搜索接口模板(含 {q} 占位符); 空 = 内置源(Bing RSS)
    // HTTP 代理(http / https 都走它, https 走 CONNECT 隧道); host 空 = 不用代理
    std::string proxyHost;
    int proxyPort = 0;
    std::string proxyUser;
    std::string proxyPassword;

    // ---- MCP 客户端(连外部 MCP 服务器, 把远端工具挂进本 agent) ----
    //   见 JZCliMcpServer: 加减 server 只改配置, 不用重编译; 启动时连一次, 之后不再变。
    bool mcpEnabled = true;
    std::vector<JZCliMcpServer> mcpServers;

    std::string shellProgram;
    int recycleSec = 15 * 60;
    std::string logFile;

    // 程序映射: 程序名 -> 可执行文件全路径(动态登记, 见 JZVirtualTerminalConfig::programPaths)
    //   典型是 tsc / code_runtime 这类"随部署走、通常不在 PATH 里"的程序 —— 登记后:
    //   进白名单(短名字能过审) + 执行时把命令里的短名换成全路径(短名字能找到程序)。
    //   ⚠ 只认短名字: 写全路径走路径审核(区外拒), 需要就另外把它配进 rootDir/trustedDirs。
    //   由外部(配置文件 terminal.programs / 宿主代码)传入; 没登记的程序 agent 就用不了。
    std::map<std::string, std::string> programPaths;

    // ---- HTTP 传输 ----
    std::string httpBind = "127.0.0.1";
    int httpPort = 8080;
    std::string httpToken;             // 非空则要求 ?token= 或 Authorization: Bearer(防裸奔)
    std::string httpDir;               // 网页静态目录; 空=exe 同目录/http

    // ---- FRP 内网穿透(frps 服务端 + frpc 客户端; "server" 角色即 frps) ----
    //   默认都关; 需要时打开 frp.server.enabled(本端当 frps 中转) 或 frp.client.enabled
    //   (本端当 frpc 把本地服务暴露到远端 frps)。详见 JZCliFrpConfig。
    JZCliFrpConfig frp;

    // ---- 控制面(managed 模式): 连 agent_cli_server ----
    JZCliServerLinkConfig server;

    // managed = 受 server 控制: 不进 HTTP、不起 frp、任务只从控制面来
    bool managed() const { return !server.host.empty(); }
};

// ---------------------------------------------------------------------------
// JZCliToolHost —— agent_cli 侧的工具宿主
//   给 agent_tool 的通用工具喂区域配置 + 读文件(直读, 无缓存)。
//   与 agent_node 的 JZAgentNodeHost 同一套逻辑, 只是配置来自 JZCliConfig。
// ---------------------------------------------------------------------------
class JZCliToolHost : public ICodeToolHost
{
public:
    explicit JZCliToolHost(const JZCliConfig *config);

    void refresh();

    const JZVirtualTerminalConfig &toolTerminalConfig() const override { return m_terminalConfig; }
    bool toolReadFile(const std::string &absPath, std::string &text, std::string &error) override;

private:
    const JZCliConfig *m_config = nullptr;
    JZVirtualTerminalConfig m_terminalConfig;
};

// 一个 MCP 服务器在本进程里的状态: 配置里配了几个就有几条(按配置顺序)。
// 连接是异步的, 所以它反映的是"当前"而不是"配置" —— 供 /info 与日志自检。
struct JZCliMcpState
{
    std::string name;
    std::string transport;   // stdio / http
    std::string target;      // stdio: 命令行; http: url(只作展示)
    bool ready = false;      // 连接成功且远端工具已登记进 agent
    int tools = 0;           // 登记到的远端工具数
    std::string error;       // 失败原因(连不上 / 挂载被拒); 空 = 没出错
};

// 本轮已发生的工具调用(供 SSE 连接时做当前状态快照)
struct JZCliToolSnap
{
    std::string id;
    std::string name;
    std::string args;
    bool hasResult = false;   // 已收到 tool_result?
    bool ok = false;          // 结果是否成功(仅 hasResult 时有效)
    std::string result;       // 工具返回(仅 hasResult 时有效)
};

// 最近任务的一条记录(长程任务: 网页不会一直开着, 重连时靠 hello / GET /info 的 recent 补回界面)
struct JZCliHistoryItem
{
    int seq = 0;
    std::string prompt;        // 请求原文("最近的请求")
    std::string verifyCmd;
    std::string status;        // running / success / failed / cancelled
    std::string reply;         // agent 最终回复(结论)
    std::string verifyOut;     // 最后一次验证输出
    std::string error;
    int rounds = 0;            // 验证轮次
    int64_t startMs = 0;
    int64_t endMs = 0;
};

// ---------------------------------------------------------------------------
// JZAgentCli —— 通过 HTTP 暴露任务提交的 agent
//
// 传输: HTTP server。外部通过 POST /task 提交任务(JSON: prompt / images / verify_script /
// verify_cmd / verify_timeout / max_verify_rounds / max_retries / max_fix_retries /
// retry_delay_ms / use_memory / token?), 本端跑 JZAgent, 每轮结束跑验证脚本(返回 0 通过 / 1 失败),
// 失败把输出喂回 agent 自动续跑, 直到验证通过或达最大轮次, 再一次性回 JSON
// {status, verify_rounds, retries, agent_reply, last_verify_output, ...}。
// agent 中途报错不会直接判失败: 在(分档的)重试预算内自动"接着干"(见 RetryMode)。
//
// images = 附带的图片(多模态): 数组, 元素是 dataURL(截图那种) 或 http(s) 链接, 与 prompt
//   同属一条 user 消息(交给 JZAgent 的 requestWithImages, 与 code_agent 桌面版截图按钮同一条路)。
//   也接受 OpenAI 的 messages 写法(content 数组里的 {"type":"image_url"})。
//   文字与图片都空才算空任务 —— 允许"只发截图"。
//
// 工具由本端**本地执行**: JZAgent 直接调用 agent_tool 里的工具(走 JZAgentTool 接口),
// 不经过 MCP。所以 agent_cli = "没有 UI 的 code_agent" + HTTP 传输层。
// ---------------------------------------------------------------------------
class JZAgentCli : public JZObject
{
public:
    explicit JZAgentCli(JZObject *parent = nullptr);
    ~JZAgentCli() override;

    // 建 JZAgent、注册工具、设 system prompt。必须在事件循环起来前调用。
    bool init(const JZCliConfig &config, std::string &error);

    // 起 HTTP 服务: 暴露 POST /task 提交任务 + 验证脚本门控循环。
    void startHttp();

    // 按配置(nodes[])连接各 link 模式 agent_node(异步; 断线自动重连)。
    // 必须在事件循环起来前调用; 没配 node 时什么都不做(纯本地执行)。
    void startNodes();

    // 起控制面(managed 模式): JZNetClient 连 server, 上报 hello/心跳, 收 taskSubmit/taskCancel/reset。
    // 必须在事件循环起来前调用; 未配置 server.host 时什么都不做(standalone)。
    // ⚠ managed 下**不**调 startHttp()/startFrpc(): 任务只从控制面来, 对外不开 HTTP。
    void startServerLink();
    bool hubOnline() const;
    JZHubLink *hub() const { return m_hub; }

    // remote_ssh 的 host -> 连接(找不到返回 nullptr); 供工具与自检使用
    JZNodeLink *nodeLink(const std::string &name) const;
    // 节点状态(JSON 数组): /info、控制面 hello/心跳、管理台共用同一份口径
    Json::Value nodesJson() const;
    // host 省略时用哪台(配置里 default 的那台; 没标就是第一台)
    std::string defaultNodeName() const;

    // 起 FRP 穿透(client 角色 = frpc): 按配置连到远端 frps, 把本地服务经代理暴露出去。
    // 必须在事件循环起来前(建 HTTP 之后)调用; 内部创建 JZFrpClient 并异步连接。
    // ⚠ 服务端(frps)由 JZAgentCliServer(见 server/)承担, 本类只做客户端。
    void startFrpc();

    // 起 MCP 客户端: 按配置(mcp.servers)逐个**异步**连接外部 MCP 服务器, 连上后把
    // 远端工具登记进 agent(见 JZMcpClient / JZAgent::addMcp)。
    // 要在事件循环起来前调用(越早发起越好), 连接结果异步到达: 失败只记日志, 不影响启动;
    // 连接晚于首个任务则挂不上(JZAgent 只在 Inactive 允许改工具表), 只记一条警告。
    void startMcp();

    // /task 请求入口(由内部 TaskHandler 调用)
    void onTaskSubmit(JZHttpServerClient *client, const JZHttpRequest &req);

    // 网页控制台: 列出工作区内可当验证脚本的文件(GET /scripts)
    void onListScripts(JZHttpServerClient *client, const JZHttpRequest &req);

    // 网页控制台: 运行信息(GET /info) —— 顶栏显示模型地址等; SSE hello 帧带同一份数据
    //   {agent, version, modelUrl, modelName, rootDir, busy, round, mcp[]}。刻意不含 modelToken。
    //   mcp[] = 每个配置的 MCP 服务器当前状态(挂上了没有/几个工具/什么错), 没网页面板时
    //   可以用它自检(curl /info)。
    Json::Value makeInfo() const;
    void onInfo(JZHttpServerClient *client, const JZHttpRequest &req);

    // 网页控制台: 重置(POST /reset)。body: {"scope":"task"|"history"}
    //   task    = 中止当前任务(在途请求回 status:"cancelled") + 重置会话
    //   history = 只重置会话(清对话历史), 不动在途任务
    // 两者最终都调 JZAgent::reset(): 清 LLM 上下文/用量/待发请求, 下次提交从干净起点开始。
    void onReset(JZHttpServerClient *client, const JZHttpRequest &req);

    // 网页控制台: 实时事件流(GET /events, Server-Sent Events)
    //   把 agent 的 sigError / 验证结果 / 任务起止实时推给网页 —— 否则任务跑很久时
    //   界面只能干等到 /task 的一次性回包, 中途报错无从得知。
    //   帧格式: data: {"type":"...","data":{...}}
    //   type: hello / state / task_start / thinking / reply / tool_call / tool_result /
    //         error / verify / task_end / reset
    //   state: 新连接时若正有任务在跑, 推一份当前状态快照(让刚打开网页的人看到进行中的任务);
    //          data = {busy, round, verify_cmd, prompt, reply, think, last_verify_output, tools[]}
    void onEvents(JZHttpServerClient *client, const JZHttpRequest &req);
    void pushEvent(const std::string &type, const Json::Value &data);   // 推给所有订阅者
    void removeEventClient(JZHttpServerClient *client);

    // 网页"后台任务"面板(给人看的, 不占模型的上下文):
    //   GET  /terminal[?tail=]              -> {running, runs:[{run_id,command,status,...}]}
    //   GET  /terminal/log?run_id=&tail=&grep=&stream= -> {run_id,status,text,...}
    //   POST /terminal/cancel?run_id=...    -> {ok,run_id}  (run_id 省略 = 停所有在跑的)
    void onTerminalList(JZHttpServerClient *client, const JZHttpRequest &req);
    void onTerminalLog(JZHttpServerClient *client, const JZHttpRequest &req);
    void onTerminalCancel(JZHttpServerClient *client, const JZHttpRequest &req);

    // 任务提交核心: 由 HTTP(onTaskSubmit) 调用, 解析 prompt / 验证脚本 / 轮次, 进入"提交→验证"循环。
    // respond 在循环结束(成功/失败/取消)时被调用一次, 带回 JSON 结果
    //   {status, verify_rounds, agent_reply, last_verify_output, error?}。
    void submitTask(const Json::Value &req, std::function<void(const Json::Value &)> respond);

    JZAgent *agent() const { return m_agent; }

    // 生效的程序映射(即配置里传进来的那份), 供宿主打印/自检
    const std::map<std::string, std::string> &programPaths() const { return m_config.programPaths; }

protected:
    void setupAgent();
    std::string buildSystemPrompt() const;

    // 一次 MCP 连接结束(成功/失败都到这儿): 就绪则把它挂进 agent(登记远端工具),
    // 失败/挂不上则记日志并销毁这个 client。index = 它在 m_config.mcpServers 里的位置
    // (用来回填 m_mcpStates)。末尾会调 finishMcpPending() 记账。
    void onMcpFinished(JZMcpClient *client, int index);
    // 一个 MCP 连接流程结束了: 未结束数 -1, 减到 0 表示"MCP 初始化完毕"(放行新任务)。
    void finishMcpPending();

    // ---- 长期记忆(跨任务) ----
    // 记忆文件全路径: 配置 memory.file 优先(相对路径按 rootDir 解析), 空则 <rootDir>/.agent_memory.md
    std::string memoryFilePath() const;
    // 开工时注入任务消息的记忆头部(未启用/还没有记忆时返回空串)。注入与 memory read 同一份逻辑。
    std::string buildMemoryBlock() const;

    std::string extractUserText(const Json::Value &params);
    // 收集 /task 的图片附件(多模态): images 数组 或 messages 里的 image_url 部分。
    // 返回可直喂模型的 URL 列表; 有格式/大小/数量问题时写 error(调用方据此回 400)。
    std::vector<std::string> extractImages(const Json::Value &params, std::string &error);

    // JZAgent 信号
    void onReply(const std::string &text);
    void onThinking(const std::string &text);
    void onToolCall(const ToolCall &call);
    void onToolResult(const ToolCallResult &result);
    void onError(const std::string &err);
    void onFinished();

    // ---- 出错自动重试(在重试预算内"接着发继续任务") ----
    // 分两档:
    //   Transient 瞬时错误(连接失败/超时/5xx/轮次用尽): 原样"继续"就行, 预算 maxRetries;
    //   Fix       可修复错误(4xx 请求被拒/上下文撑爆): 原样重发必然再失败, 改成"让模型换做法"
    //             (重发合法的工具调用 / 别整段贴文件), 预算 maxFixRetries;
    //   None      用户取消等: 不重试, 直接判失败。
    enum RetryMode { RetryNone = 0, RetryTransient = 1, RetryFix = 2 };
    static RetryMode classifyError(JZAgent::AgentError code, const std::string &err);
    std::string buildRetryPrompt(RetryMode mode, JZAgent::AgentError code, const std::string &err) const;
    void scheduleErrorRetry(RetryMode mode);

    // 有没有 SSE 订阅者: 没有就不必构造高频的流式事件(thinking/reply)JSON
    bool hasEventConsumer() const { return !m_eventClients.empty(); }

    // HTTP 传输 + 验证脚本门控循环
    std::string deriveVerifyCmd(const std::string &script, const std::string &explicitCmd) const;
    // 从候选里挑一个"本机真能跑"的解释器(配置 programPaths 优先, 否则探测):
    // Windows cmd 下 python3 / bash 常常不存在, 硬写会让验证脚本永远起不来。
    std::string resolveProgram(const std::vector<std::string> &cands, const std::string &fallback) const;
    // 跑一次验证命令。**异步**: 起进程后立刻返回, 结果经 cb(exitCode, output) 回调。
    // 为什么不用 _popen: ① 它是同步的, 会卡住主线程(SSE/HTTP/定时器全停, 网页看着像挂了);
    // ② Windows 下根本没有超时手段(以前那段 timeout 包装是 #ifndef _WIN32) ⇒ 验证命令
    //    一旦不退出, 整个 agent_cli 永久卡死, verify_timeout 形同虚设。
    // 现在统一用 JZProcess + JZTimer 超时(到点连进程树一起杀, 见 JZProcess 的作业对象)。
    void runVerifyAsync(const std::string &cmd, int timeoutMs, const std::string &cwd,
                        const std::function<void(int, const std::string &)> &cb);
    void killVerify();                       // 停掉在跑的验证进程(重置/收尾时用)
    void finishVerify(int exitCode);         // 一次验证的收尾(截断输出 + 回调)
    void onVerifyDone(int exitCode, const std::string &out);   // 验证结果的分发(正常路径)
    void onLoopFinished();

    void respondTask(const std::string &status, int rounds, const std::string &reply,
                    const std::string &verifyOut, const std::string &err);
    bool checkToken(const JZHttpRequest &req) const;      // ?token= / Authorization: Bearer
    void resetSession(bool cancelRunning);                // 重置会话(可选先中止在途任务)
    // 中止在途任务并复位会话(返回是否真的中止了一个任务)。HTTP 的 /reset 与控制面的
    // taskCancel 走同一条路 —— 两条入口各写一套必然漏掉某处收尾(验证进程/后台命令)。
    bool cancelTask(const std::string &reason);

    // 控制面下发(由 JZHubLink 回调)
    void onHubTaskSubmit(const std::string &taskId, const Json::Value &body);
    void onHubTaskCancel(const std::string &taskId);
    void onHubReset(const std::string &scope);
    std::string nodesJsonString() const;
    void endLoop();
    void onClientDestroyed(JZObject *);
    void sendJson(JZHttpServerClient *client, int code, const Json::Value &j);

    JZCliConfig m_config;
    JZCliToolHost m_host;
    std::vector<JZNodeLink *> m_nodeLinks;    // 按 nodes[] 顺序(启动时建; 本对象所有)
    std::vector<JZCliMcpState> m_mcpStates;   // 每个 MCP server 的连接/挂载状态(见 startMcp)
    // MCP 初始化闸门: 配置里的 server **全部有结果**(连上 / 连不上)之前拒绝新任务 ——
    // 否则任务先跑起来, 稍后连上的远端工具就挂不进 agent 了(只能重启进程, 见 onMcpFinished)。
    bool m_mcpInitDone = true;   // 没配 MCP(或没启用)时恒为 true
    int m_mcpPending = 0;        // 还在连接中的 server 数(发起就失败的算已结束, 不计数)
    JZVirtualTerminal *m_terminal = nullptr;
    // 联网工具(web_search / web_fetch)共享的访问管理器 —— 代理只在它上面设一次。
    // 是本对象的子对象; 没挂网络工具时为空。
    JZNetworkAccessManager *m_netMan = nullptr;
    JZAgent *m_agent = nullptr;
    std::vector<JZAgentTool *> m_tools;
    JZMemoryTool *m_memory = nullptr;    // 非拥有(m_tools 持有); 未启用记忆时为空
    int m_systemPromptBytes = 0;         // 启动时算一次的 system prompt 大小(GET /info 自检用)

    std::atomic<bool> m_busy{false};
    std::atomic<bool> m_failed{false};
    std::atomic<bool> m_cancelled{false};
    std::string m_lastError;

    // 出错重试状态(每个任务一份: submitTask/resetSession 里清零)
    JZAgent::AgentError m_lastErrorCode = JZAgent::None;  // 本次错误分类, sigError 时从 agent 取
    int m_errorRetries = 0;          // 本任务已用掉的"瞬时错误"重试次数
    int m_maxErrorRetries = 0;       // 瞬时错误预算(来自配置, 可被 /task 的 max_retries 覆盖)
    int m_fixRetries = 0;            // 本任务已用掉的"可修复错误"重试次数
    int m_maxFixRetries = 0;         // 可修复错误预算(可被 /task 的 max_fix_retries 覆盖)
    int m_retryDelayMs = 1000;       // 重试等待(ms), 可被 /task 的 retry_delay_ms 覆盖
    bool m_retryPending = false;     // 已排队一次延时重试, 等 JZTimer 触发
    int m_loopEpoch = 0;             // 任务世代号: 新任务/重置时 +1, 作废在途的延时重试

    // HTTP 传输
    JZHttpServer *m_http = nullptr;

    // 控制面(managed 模式): 到 server 的连接。standalone 下为空。
    JZHubLink *m_hub = nullptr;
    // 控制面任务 id(server 生成)。非空 = 当前任务是 server 派下来的,
    // 事件流与回包都按它带回 server; HTTP 任务为空。
    std::string m_currentTaskId;

    // FRP 穿透(client 角色 = frpc; 服务端 frps 在 JZAgentCliServer)
    //   按配置创建 JZFrpClient, 连到远端 frps 暴露本地服务; 本对象的子对象。
    JZFrpClient *m_frpClient = nullptr;
    JZHttpServerClient *m_pendingClient = nullptr;   // 当前在跑任务对应的 HTTP 连接(循环结束回包)
    bool m_loopActive = false;                       // 是否处于"提交→验证循环"模式
    int m_loopRound = 0;
    int m_maxLoopRounds = 10;
    std::string m_verifyCmd;                         // 推导好解释器的验证命令
    int m_verifyTimeoutMs = 60000;
    std::string m_accumReply;                        // 累积本轮 agent 最终回复
    std::string m_lastVerifyOutput;
    // 在跑的验证进程(异步): 收尾/重置时要能把它连同子孙一起停掉
    JZProcess *m_verifyProc = nullptr;
    bool m_verifyRunning = false;
    bool m_verifyTimedOut = false;
    int m_verifyGen = 0;                             // 世代号: 作废上一轮验证的定时器/信号
    std::string m_verifyOut;                         // 本次验证的累积输出(共用 stdout/stderr)
    // Windows: 命令写进的临时 .cmd 路径(见 runVerifyAsync 的说明), 收尾时删掉
    std::string m_verifyScript;
    // 分发用的回调: 起进程时存下来, sigFinished 里调
    std::function<void(int, const std::string &)> m_verifyCb;

    // 最近一次任务的回包。提交方(POST /task 的那条连接)断连后任务仍在跑完 —— 那时结果只能
    // 经 SSE 出口; 若订阅者也恰好不在(网页关了又开), 重连时的 hello 帧带上它, 让界面收尾。
    Json::Value m_lastResult;
    bool m_hasLastResult = false;

    // 当前状态快照(SSE 新连接时推过去, 让刚打开网页的人看到正在跑的任务)
    std::string m_currentPrompt;                     // 当前任务 prompt
    std::string m_accumThink;                        // 累积本轮思考(thinking)
    bool m_thinkLogged = false;                    // 本轮 think 是否已落 Agent 日志(每单元一条)
    std::vector<JZCliToolSnap> m_toolSnaps;          // 本轮已发生的工具调用/结果
    std::string m_lastFeedbackSig;                   // 上一轮"喂回模型"的失败签名(退出码+输出)
    int m_sameFailCount = 0;                         // 连续拿到完全一致失败的次数(识别原地打转)
    bool m_sessionStarted = false;                   // 是否已发起过首轮 request(避免首轮 reset 告警)

    // 最近任务历史: 长程任务跑几十分钟很常见, 网页不会一直开着 —— 服务端记住最近若干次
    // 请求与它们的结果, 新连接(hello 帧)或 GET /info 时以 recent 数组吐出去, 界面据此重建。
    std::vector<JZCliHistoryItem> m_history;   // 最旧在前, 上限 kHistoryMax
    int m_historySeq = 0;
    static const int kHistoryMax = 20;
    void pushHistory(const std::string &prompt, const std::string &verifyCmd);
    void finishHistory(const std::string &status, int rounds, const std::string &reply,
                       const std::string &verifyOut, const std::string &err);
    Json::Value recentJson() const;            // 最近在前(与界面"新的在上"一致)
    static int64_t nowMs();

    // 传输无关扩展
    std::function<void(const Json::Value &)> m_respond;  // 循环结束的回包回调

    // SSE 订阅者(GET /events): 由连接析构回调摘除
    std::vector<JZHttpServerClient *> m_eventClients;
};

#endif // !JZ_AGENT_CLI_H_
