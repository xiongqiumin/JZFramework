#include "JZAgentNode.h"

#include "JZByteArray.h"
#include "JZDebug.h"
#include "JZDir.h"
#include "JZFile.h"
#include "JZFileInfo.h"
#include "JZMcpCore.h"
#include "JZMcpServer.h"
#include "JZMcpToolBridge.h"
#include "JZDateTime.h"
#include "JZNet/JZNetServer.h"
#include "JZNet/JZNetPacket.h"
#include "JZTerminalTools.h"
#include "JZTerminalMcpTools.h"
#include "JZTimer.h"
#include "JZVirtualTerminal.h"

using namespace std;

// 单次读文件的上限。工具自己还会按行裁剪, 这里只是避免把几百兆的东西读进内存。
static const int kReadFileLimit = 8 * 1024 * 1024;

// 一次 exec 回给 cli 的输出上限。协议单包上限是 256KB(MG_MAX_PACK_SIZE), 这里留出余量:
// 头尾各留一半 + 一行显式标记 —— 与 terminal 的"上下文阀"同一个思路(可恢复的截断,
// 而不是静默丢内容)。
static const size_t kMaxExecOutputBytes = 120 * 1024;

// 按字符边界裁(UTF-8 多字节字符不能被切成半个, 否则后续写进 JSON 会坏掉)
static size_t utf8Boundary(const string &text, size_t pos)
{
    while (pos > 0 && pos < text.size() && ((unsigned char)text[pos] & 0xC0) == 0x80)
        --pos;
    return pos;
}

// grep 过滤: ASCII 不区分大小写(中文原样比较)。grep 是模型随手写的关键字,
// 让它因为大小写不同一无所获没有任何好处。
static bool containsNoCase(const string &text, const string &pat)
{
    if (pat.empty())
        return true;
    if (pat.size() > text.size())
        return false;

    auto lower = [](char c) -> char
    {
        return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    };

    for (size_t i = 0; i + pat.size() <= text.size(); ++i)
    {
        size_t k = 0;
        while (k < pat.size() && lower(text[i + k]) == lower(pat[k]))
            ++k;
        if (k == pat.size())
            return true;
    }
    return false;
}

static string cutExecOutput(const string &text)
{
    if (text.size() <= kMaxExecOutputBytes)
        return text;

    size_t keep = kMaxExecOutputBytes / 2;
    size_t headEnd = utf8Boundary(text, keep);
    size_t tailStart = utf8Boundary(text, text.size() - keep);
    string head = text.substr(0, headEnd);
    string tail = text.substr(tailStart);
    size_t dropped = tailStart - headEnd;

    return head + "\n...[输出过长: 省略中间 " + to_string(dropped) + " 字节(共 " +
           to_string(text.size()) + " 字节); 要全文请自己在命令里过滤或写到文件里再分段读]...\n" + tail;
}

// ==========================================================================
// JZAgentNodeHost
// ==========================================================================

JZAgentNodeHost::JZAgentNodeHost(const JZNodeConfig *config)
    : m_config(config)
{
}

void JZAgentNodeHost::refresh()
{
    // 默认策略: 工作区读+写+执行, 授信目录读+执行, 系统临时目录读+写, 区外只读。
    m_terminalConfig = JZVirtualTerminalConfig();
    m_terminalConfig.zone.workspace = m_config ? m_config->rootDir : string();
    m_terminalConfig.zone.trustedDirs = m_config ? m_config->trustedDirs : vector<TrustedDir>();
    // 包管理 / 联网两道闸: 默认关(见 JZNodeConfig 的注释), 开了才放行。
    // 终端与文件工具共用这一份配置(文件工具的写权限判定走的也是它), 所以这里
    // 是"这台机器的策略"唯一的事实来源。
    if (m_config)
    {
        // 权限等级: 全部不检查时, 下面这些开关就不看了(见 JZVirtualTerminalConfig::level)
        m_terminalConfig.level = m_config->unrestricted ? AccessLevel::Unrestricted
                                                       : AccessLevel::Standard;
        m_terminalConfig.allowPackageManager = m_config->allowPackageManager;
        m_terminalConfig.allowNetwork = m_config->allowNetwork;
    }
}

bool JZAgentNodeHost::toolReadFile(const std::string &absPath, std::string &text, std::string &error)
{
    JZFileInfo info(absPath);
    if (!info.exists())
    {
        error = "文件不存在: " + absPath;
        return false;
    }
    if (info.isDir())
    {
        error = "这是一个目录(用 list_dir): " + absPath;
        return false;
    }
    if (info.size() > kReadFileLimit)
    {
        error = "文件过大(>8MB), 读不了: " + absPath + "  可以改用终端里的命令按需处理";
        return false;
    }

    JZFile file;
    if (!file.open(absPath, "rb"))
    {
        error = "打开文件失败(权限?): " + absPath;
        return false;
    }
    JZByteArray data = file.readAll();
    file.close();

    text.assign(data.data(), data.size());
    return true;
}

// ==========================================================================
// JZAgentNode
// ==========================================================================

JZAgentNode::JZAgentNode(JZObject *parent)
    : JZObject(parent)
    , m_host(&m_config)
{
}

JZAgentNode::~JZAgentNode()
{
    // 先收掉 server(连带它持有的 bridge 与工具), 再轮到 m_config 析构 ——
    // 工具里存的是 &m_config / &m_host, 虽然析构路径上不会解引用, 但显式断掉更稳。
    delete m_server;
    m_server = nullptr;
}

void JZAgentNode::normalizeConfig(JZNodeConfig &config)
{
    if (config.nodeName.empty())
        config.nodeName = "agent_node";
    if (config.rootDir.empty())
        config.rootDir = JZDir::currentPath();

    // 统一成 "/" 分隔的绝对路径: 区域判定是纯字符串前缀比较, 不规范化会对不上
    config.rootDir = codeNormalizePath(config.rootDir);
    if (config.rootDir.empty())
        config.rootDir = codeNormalizePath(JZDir::currentPath());

    for (TrustedDir &dir : config.trustedDirs)
        dir.path = codeNormalizePath(dir.path);
}

void JZAgentNode::init(const JZNodeConfig &config)
{
    m_config = config;
    normalizeConfig(m_config);
    m_host.refresh();

    m_server = new JZMcpServer(this);
    m_server->core()->setServerInfo(m_config.nodeName, m_config.version);
    m_server->core()->setInstructions(buildInstructions());

    // ---- 文件工具 ----
    // read_file / write_file / modify_file 是 agent_tool 里现成的那份(与本地
    // code_agent 同一份实现): 按行读带行号、可分段, 改代码校验唯一匹配并回上下文
    // 片段 —— 这几件事 shell 给不了, 所以不走终端。
    if (m_config.allowFileTools)
    {
        addTool(new JZReadFileTool());
        addTool(new JZWriteFileTool());
        addTool(new JZModifyFileTool());
    }

    // ---- 终端: 这台机器上唯一的执行面与审核面 ----
    // ls / cat / mkdir / rm / cp / mv / grep 都不再单独做工具 —— 它们是 shell 命令,
    // 由 terminal_run 直接覆盖。
    // 终端按会话管理(map<session, JZVirtualTerminal>, 见 JZTerminalRegistry):
    // 每个会话一个独立终端实例, 由 terminal_open 按需创建(见 open())。
    if (m_config.allowTerminal)
    {
        // terminal 的 MCP 直出门面(不经过 agent_tool 包装):
        //   terminal_run    —— 支持 stream(内联 SSE 流式日志)/ background(立即返回 run_id)
        //   terminal_query  —— 取后台/历史运行日志(支持 tail / stream / grep 过滤)
        addTool(new JZTerminalRunMcpTool(this));
        addTool(new JZTerminalQueryMcpTool(this));
        addTool(new JZTerminalOpenMcpTool(this));
        addTool(new JZTerminalListMcpTool(this));
        addTool(new JZTerminalCloseMcpTool(this));
    }

    if (!m_config.allowFileTools && !m_config.allowTerminal)
        jzWarning() << "allowTerminal 与 allowFileTools 都是 false: 这个节点一个工具都没注册";

    // 空闲会话的回收: 只有开了终端才有会话可管
    startRecycleTimer();

    jzInfo().noquote().nospace() << "agent_node 就绪: name=" << m_config.nodeName
                                 << " root=" << m_config.rootDir
                                 << " shell=" << (m_config.shellProgram.empty() ? string("auto(/bin/sh)") : m_config.shellProgram)
                                 << " 会话上限=" << m_config.maxSessions
                                 << " 空闲回收=" << (m_config.recycleSec > 0 ? to_string(m_config.recycleSec) + "s" : string("关"))
                                 << " 包管理=" << (m_config.allowPackageManager ? string("开") : string("关"))
                                 << " 联网=" << (m_config.allowNetwork ? string("开") : string("关"));
}

void JZAgentNode::addTool(JZAgentTool *tool)
{
    // 通用工具(agent_tool 里那批)需要一个宿主才能做区域判定/读文件。
    // 漏了这一步的话 checkPathPermission 会因为"没有宿主"直接放行,
    // 区域约束等于没开 —— code_agent 那边是在 addTool 里 setHost 的。
    JZCodeTool *codeTool = dynamic_cast<JZCodeTool *>(tool);
    if (codeTool)
        codeTool->setHost(&m_host);

    // bridge 接管工具所有权; registerTool 再接管 bridge(挂到 core 下)
    m_server->core()->registerTool(new JZMcpToolBridge(tool));
}

void JZAgentNode::addTool(JZMcpTool *tool)
{
    // terminal 的 MCP 直出门面: 直接挂进 core, 不经过 agent_tool 包装。
    m_server->core()->registerTool(tool);
}

// ⚠ 说明是"给模型看的说明书", 必须和 init() 里真正注册的工具一一对应 ——
// 写死一份的话, 关掉某个开关后模型还在按不存在的工具规划, 只会白试。
string JZAgentNode::buildInstructions() const
{
    string text;
    text += "你是运行在主机 \"" + m_config.nodeName + "\" 上的操作节点。\n";
    text += "工作区: " + m_config.rootDir + "  —— 相对路径以它为基准。\n";
    text += "写权限: 工作区(读+写+执行)、系统临时目录(读+写, 如 /tmp)、授信目录(默认只读, "
            "单独开了可写的才能写); 别处一律只读 —— 往那里写会被拒绝。\n";
    text += "命令策略: " + string(m_config.allowPackageManager ? "允许" : "禁止") +
            "装/升依赖(pip/npm/apt...), " + string(m_config.allowNetwork ? "允许" : "禁止") +
            "联网下载(curl/wget...); 破坏性命令(dd/sudo/shutdown...)一律拒绝。\n\n";

    text += "可用工具:\n";
    if (m_config.allowTerminal)
    {
        text += "  terminal_run    执行一条命令。命令**整条交给 shell**, "
                "管道(|)、重定向(> >>)、串联(&& || ;)、通配符(*)、变量($VAR) 都按 shell 的语义工作。\n";
        text += "  terminal_open / terminal_list / terminal_close   多会话管理。⚠ terminal_run "
                "**必须先 terminal_open** 拿到 session id 才能跑(节点不自动开默认会话)。\n";
        text += "  terminal_query   取后台/历史运行的日志(见下方 background / 流式说明)。\n";
        if (m_config.maxSessions > 0)
        {
            text += "    会话上限 " + to_string(m_config.maxSessions) + " 个, 空闲 " +
                    to_string(m_config.recycleSec) + " 秒会被自动回收 —— 不用了就 terminal_close, "
                    "别靠回收清理。\n";
        }
    }
    if (m_config.allowFileTools)
    {
        text += "  read_file / write_file   按行读(带行号, 可分段) / 整文件写入。\n";
        text += "  modify_file     精确字符串替换(要求唯一匹配); 改代码用这个, **不要用 sed**。\n";
    }
    if (!m_config.allowTerminal)
        text += "  ⚠ 本节点没开终端: 文件操作(ls/cat/cp/grep...)都没有了, 只能用上面的文件工具。\n";
    text += "\n";

    if (!m_config.allowTerminal)
        return text;

    text += "terminal_run 的两种进阶用法:\n";
    text += "  - 流式(stream=true): 长命令会边跑边把日志以 notifications/message 推过来"
            "(POST 响应内联 SSE, 结束再给结果), 适合编译/跑测试这类慢慢出日志的命令。"
            "可配 stream_filter(stdout/stderr/all) 与 grep 只推关心的行。\n";
    text += "  - 后台(background=true): 命令在后台跑, 立刻返回 run_id(不流式、不带日志);"
            "之后用 terminal_query 取日志。可配 tail(最近 N 行)、stream、grep 过滤。\n";
    text += "    前台命令的结果末尾也会回一行 `[run_id]`, 可用 terminal_query 复查该次运行的完整/历史日志。\n\n";

    // ⚠ 这段是这个节点最重要的一段说明: 模型的 shell 直觉在这里只有一部分成立。
    // ⚠ 终端已换成有状态解释器(JZVirtualTerminal 持有同一个 JZShell): cd/export 跨命令保留,
    //   不能再写"每条命令一个新进程"(会把模型带偏)。
    text += "⚠ 每个会话是**一个有状态的 shell**(命令在同一个解释器里跑):\n";
    text += "  - `cd` / `export` 会保留到下一条命令; `cd X && <命令>` 之后相对路径按 X 解析,"
            "权限也按 X 判 —— cd 到工作区外(临时目录除外)依然写不了。\n";
    text += "  - 不确定当前目录就结果末尾那行 `[cwd]`(单独的 `pwd` 也返回它);"
            "想回工作区根就 `cd <工作区根>`。\n";
    text += "  - 单前台任务: 上一条没结束, 再跑一条会直接报忙(不排队)。\n";
    text += "  - heredoc、`$( )`/反引号、进程替换都能用; 审核只看**能静态解析出来的命令**"
            "(白名单 / 区域权限 / 危险指令), 藏在 `$( )` 或 heredoc body 里的命令不审、按可信处理。\n";
    text += "  - 不支持: 后台 `&`、nohup/setsid/screen, 以及 for/while/if 等复合语句。\n";
    text += "  - 交互式程序(vim / top / REPL)跑不了 —— 子进程的 stdin/stdout 是管道, 不是 tty。\n";
    return text;
}

// ==========================================================================
// JZTerminalRegistry: 会话 -> 终端
// ==========================================================================

JZVirtualTerminal *JZAgentNode::terminal(const std::string &sessionId, std::string &error)
{
    auto it = m_terminals.find(sessionId);
    if (it == m_terminals.end())
    {
        error = "没有这个会话: " + sessionId + " (用 terminal_list 看有哪些, 或先 terminal_open)";
        return nullptr;
    }
    return it->second;
}

// 空闲会话回收(定时器周期调用 / open() 触顶时也先试一次)
// ⚠ 在跑命令的会话不动: 那是一条命令的正常耗时, 不是"没人用"。
void JZAgentNode::recycleIdleSessions()
{
    if (m_config.recycleSec <= 0)
        return;

    int64_t now = JZDateTime::currentMSecsSinceEpoch();
    std::vector<std::string> dead;
    for (auto &kv : m_terminals)
    {
        JZVirtualTerminal *t = kv.second;
        JZVirtualTerminal::SessionInfo info = t->info();
        if (info.running || info.lastActive <= 0)
            continue;
        if (t->recycleMs() <= 0)
            continue;
        if (now - info.lastActive < t->recycleMs())
            continue;
        dead.push_back(kv.first);
    }

    for (const std::string &sid : dead)
    {
        jzInfo().noquote().nospace() << "会话空闲超时, 回收: " << sid
                                     << " (空闲 " << (now - m_terminals[sid]->info().lastActive) / 1000 << "s)";
        close(sid);
    }
}

void JZAgentNode::startRecycleTimer()
{
    if (m_config.recycleSec <= 0 || !m_config.allowTerminal)
        return;
    if (m_recycleTimer)
        return;

    // 扫描周期取回收时长的 1/4, 夹在 [5s, 60s]: 15 分钟 -> 每 60s 扫一次。
    // 扫得太勤没意义(回收本来就允许几分钟的误差), 太懒则堆一堆僵尸会话。
    int gapMs = m_config.recycleSec * 1000 / 4;
    gapMs = std::max(gapMs, 5 * 1000);
    gapMs = std::min(gapMs, 60 * 1000);

    m_recycleTimer = new JZTimer(this);
    connect(&m_recycleTimer->sigTimeout, this, [this]() { recycleIdleSessions(); });
    m_recycleTimer->start(gapMs);
}

std::string JZAgentNode::open(const std::string &cwd, std::string &error)
{
    // 上限不是硬墙: 先回收掉空闲的, 还满才拒绝 ——
    // 否则模型开了一堆不关之后就再也开不了会话, 只能重启节点。
    if (m_config.maxSessions > 0 && (int)m_terminals.size() >= m_config.maxSessions)
    {
        recycleIdleSessions();
        if ((int)m_terminals.size() >= m_config.maxSessions)
        {
            error = "会话数已达上限(" + to_string(m_config.maxSessions) + "): "
                    "先用 terminal_close 关掉不用的, 或调大配置里的 maxSessions";
            return std::string();
        }
    }

    // 每个会话一个独立终端实例(各自 cwd / 前后台进程 / 运行记录);
    // JZVirtualTerminal 本身不含会话概念, 生命周期由本节点管。
    JZVirtualTerminal *t = new JZVirtualTerminal(this);
    t->setTerminalConfig(m_host.toolTerminalConfig());
    if (!m_config.shellProgram.empty())
        t->setShellProgram(m_config.shellProgram);
    if (m_config.recycleSec > 0)
        t->setRecycleMs(m_config.recycleSec * 1000);

    // 空 cwd 也要走一次 changeDir: 一是把 cwd 落成工作区根的绝对路径(否则会话的
    // cwd 是空的, 工具回的 [cwd] 行没内容), 二是它会刷新 lastActive ——
    // 不刷新的话这个会话"从 1970 年就没动过", 下一轮扫描直接被回收掉。
    if (!t->changeDir(cwd, error))
        return std::string();   // changeDir 已填 error(越界等)

    std::string sid = "t" + std::to_string(++m_sessionIdGen);
    t->setId(sid);
    m_terminals[sid] = t;

    jzInfo().noquote().nospace() << "开会话 " << sid << " cwd=" << t->info().cwd
                                 << " (当前 " << m_terminals.size() << "/" << m_config.maxSessions << ")";
    return sid;
}

bool JZAgentNode::close(const std::string &sessionId)
{
    auto it = m_terminals.find(sessionId);
    if (it == m_terminals.end())
        return false;
    JZVirtualTerminal *t = it->second;
    m_terminals.erase(it);
    jzInfo().noquote().nospace() << "关会话 " << sessionId << " (剩下 " << m_terminals.size() << ")";
    delete t;   // 析构会 kill 在跑的进程; 同时从 this 的子对象里摘掉
    return true;
}

std::vector<JZVirtualTerminal::SessionInfo> JZAgentNode::list()
{
    std::vector<JZVirtualTerminal::SessionInfo> out;
    for (auto &kv : m_terminals)
        out.push_back(kv.second->info());
    return out;
}

bool JZAgentNode::runSnapshot(const std::string &runId, JZVirtualTerminal::RunRecordInfo &out) const
{
    for (auto &kv : m_terminals)
    {
        if (kv.second->runSnapshot(runId, out))
            return true;
    }
    return false;
}

// ==========================================================================
// link 模式(配合 agent_cli): 不起 MCP, 只对外提供"执行一条命令"
//
// 角色固定: node 是 server(JZNetServer), cli 是 client(JZNetClient)。
// 审核/执行完全复用 JZVirtualTerminal —— 与 mcp 模式是同一套策略, 不写第二套。
// ==========================================================================

void JZAgentNode::initLink(const JZNodeConfig &config)
{
    m_config = config;
    normalizeConfig(m_config);
    m_host.refresh();

    // link 模式没有"多会话"概念: 一台终端, cwd 落在工作区根。
    // (mcp 模式那套 terminal_open/close 是给模型自己管会话用的, remote_ssh 不需要。)
    m_linkTerminal = new JZVirtualTerminal(this);
    m_linkTerminal->setId("link");
    m_linkTerminal->setTerminalConfig(m_host.toolTerminalConfig());
    if (!m_config.shellProgram.empty())
        m_linkTerminal->setShellProgram(m_config.shellProgram);
    // 回包截断由本文件按协议包上限做(cutExecOutput); 这里关掉终端那层 32KB 上下文阀,
    // 否则 120KB 的预算永远用不上。
    m_linkTerminal->setReplyMaxBytes(0);

    std::string dirError;
    if (!m_linkTerminal->changeDir(m_config.rootDir, dirError))
        jzWarning() << "link 模式设置工作目录失败:" << dirError;

    jzInfo().noquote().nospace() << "agent_node(link) 就绪: name=" << m_config.nodeName
                                 << " root=" << m_config.rootDir
                                 << " shell=" << (m_linkTerminal->shellProgram().empty()
                                                       ? string("auto")
                                                       : m_linkTerminal->shellProgram())
                                 << " 并发=1(单终端串行)"
                                 << " maxConcurrent=" << m_config.maxConcurrent
                                 << "(预留, 当前版本不生效)"
                                 << " 终端=" << (m_config.allowTerminal ? "开" : "关")
                                 << " 包管理=" << (m_config.allowPackageManager ? "开" : "关")
                                 << " 联网=" << (m_config.allowNetwork ? "开" : "关");
}

bool JZAgentNode::startLinkServer(std::string &error)
{
    if (m_linkServer)
        return true;
    if (m_config.token.empty())
    {
        error = "link 模式必须配 token(空 token 等于把这台机器的 shell 交给任何能连上端口的人)";
        return false;
    }

    JZAgentNodePacketInit();

    m_linkServer = new JZNetServer(this);
    m_linkServer->setToken(m_config.token);

    connect(&m_linkServer->sigAccept, this, [this]()
    {
        // 握手完成才会 emit sigAccept; 这里只把待处理队列排空(否则会一直堆着)
        uint64_t conn = m_linkServer->nextPendingConnection();
        if (conn != 0)
            jzInfo().noquote().nospace() << "node(link) 收到 cli 连接: " << conn;
    });
    connect(&m_linkServer->sigPacketReceive, this, [this](uint64_t conn, JZNetPackPtr pack)
    {
        onLinkPacket(conn, pack);
    });
    // cli 掉线(崩了/重启/网络断): 它发起的运行已经没人收结果了。
    // 不清理的话 —— 命令继续占着唯一的前台终端(新 cli 永远"节点忙"), 映射也一直残留。
    connect(&m_linkServer->sigDisconnected, this, [this](uint64_t conn)
    {
        jzInfo().nospace() << "cli 断开: conn=" << conn;
        dropLinkRuns(conn);
    });

    if (!m_linkServer->listen(m_config.host, m_config.port))
    {
        error = "link 模式监听失败 " + m_config.host + ":" + std::to_string(m_config.port) +
                "(端口被占用 / 地址不可用?)";
        m_linkServer->deleteLater();
        m_linkServer = nullptr;
        return false;
    }
    return true;
}

bool JZAgentNode::linkListening() const
{
    return m_linkServer != nullptr && m_linkServer->isListening();
}

void JZAgentNode::sendLinkResult(uint64_t conn, const JZNodePackExecResult &result)
{
    if (!m_linkServer)
        return;
    m_linkServer->sendPacket(conn, &result);
}

void JZAgentNode::onLinkPacket(uint64_t conn, JZNetPackPtr pack)
{
    if (!pack)
        return;

    const int type = pack->packetType();
    if (type == NodePack_hello)
    {
        JZNodePackHello *hello = (JZNodePackHello *)pack.get();
        JZNodePackHelloAck ack;
        ack.ok = (m_linkTerminal != nullptr);
        ack.error = ack.ok ? std::string() : std::string("节点未初始化执行终端");
        ack.nodeName = m_config.nodeName;
        ack.version = m_config.version;
        ack.rootDir = m_config.rootDir;
        ack.shell = m_linkTerminal ? m_linkTerminal->shellProgram() : std::string();
        m_linkServer->sendPacket(conn, &ack);
        jzInfo().noquote().nospace() << "cli 上线: id=" << hello->clientId << " v" << hello->cliVersion
                                     << " conn=" << conn;
        return;
    }

    if (type == NodePack_exec)
    {
        JZNodePackExec *exec = (JZNodePackExec *)pack.get();
        runLinkExec(conn, exec->runId, exec->command, exec->cwd, exec->timeoutMs, exec->background);
        return;
    }

    if (type == NodePack_termQuery)
    {
        JZNodePackTermQuery *query = (JZNodePackTermQuery *)pack.get();
        answerLinkQuery(conn, *query);
        return;
    }

    if (type == NodePack_execCancel)
    {
        JZNodePackExecCancel *cancel = (JZNodePackExecCancel *)pack.get();
        const LinkRunKey key = linkKey(conn, cancel->runId);
        auto it = m_linkTermRuns.find(key);
        if (it == m_linkTermRuns.end() || !m_linkTerminal)
        {
            // 已结束(或不是这条连接发起的): 明确忽略。**绝不能**直接 cancel():
            // 终端只有一个前台任务, 那时在跑的很可能是别人/后来的命令。
            jzInfo().nospace() << "取消: " << cancel->runId << " 已结束或不存在, 忽略";
            return;
        }

        const std::string active = m_linkTerminal->activeRunId();
        if (active != it->second)
        {
            jzInfo().nospace() << "取消: " << cancel->runId << " 已结束(当前在跑 "
                               << (active.empty() ? string("(空)") : active) << "), 忽略";
            return;
        }

        jzInfo().nospace() << "取消执行: " << cancel->runId << " (终端 " << it->second << ")";
        m_linkTerminal->cancel();
        return;
    }
}

JZAgentNode::LinkRunKey JZAgentNode::linkKey(uint64_t conn, const std::string &runId)
{
    return LinkRunKey(conn, runId);
}

void JZAgentNode::dropLinkRuns(uint64_t conn)
{
    std::vector<LinkRunKey> dead;
    for (auto &kv : m_linkTermRuns)
    {
        if (kv.first.first == conn)
            dead.push_back(kv.first);
    }

    for (const LinkRunKey &key : dead)
    {
        auto it = m_linkTermRuns.find(key);
        if (it == m_linkTermRuns.end())
            continue;
        // 只有"这条连接正在跑的那一条"才停: 共享终端上可能还有别的连接的命令
        if (m_linkTerminal && m_linkTerminal->activeRunId() == it->second)
        {
            jzInfo().nospace() << "停掉掉线 cli 的命令: " << it->first.second;
            m_linkTerminal->cancel();
        }
        m_linkTermRuns.erase(it);
    }
}

void JZAgentNode::runLinkExec(uint64_t conn, const std::string &runId, const std::string &command,
                              const std::string &cwd, int timeoutMs, bool background)
{
    auto fail = [this, conn, &runId](const std::string &why)
    {
        JZNodePackExecResult res;
        res.runId = runId;
        res.ok = false;
        res.error = why;
        sendLinkResult(conn, res);
    };

    if (!m_config.allowTerminal)
    {
        fail("节点未开放终端(allowTerminal=false)");
        return;
    }
    if (!m_linkTerminal)
    {
        fail("节点没有可用的执行终端");
        return;
    }
    // 终端只有一个前台任务: 上一条没结束就明确说"忙", 不排队。
    // ⚠ maxConcurrent 是预留字段: 单终端串行是硬约束, 配几都不改变(见 docs §6.4)。
    if (m_linkTerminal->isRunning())
    {
        fail("节点忙: 上一条命令还在跑(`" + m_linkTerminal->activeRunId() + "`), 等它结束再来");
        return;
    }

    // cwd: 空 = 会话当前目录(工作区根); 非空直接换目录 ——
    // 以前是拼 `cd "<cwd>" && <命令>`, 只拒双引号, `$()`/反引号照样会被 shell 展开,
    // 等于给 cwd 留了个绕过命令审核的口子。changeDir 自己会审权限, 也不经过 shell 解析。
    if (!cwd.empty())
    {
        std::string dirError;
        if (!m_linkTerminal->changeDir(cwd, dirError))
        {
            fail("cwd 不可用: " + dirError);
            return;
        }
    }

    const int64_t startMs = JZDateTime::currentMSecsSinceEpoch();
    const LinkRunKey key = linkKey(conn, runId);

    // 终端自己也会生成 runId(用于 cancel/查询), 与协议里的 runId 不是同一个, 这里做映射
    auto termRunId = std::make_shared<std::string>();
    auto finished = std::make_shared<bool>(false);

    auto cb = [this, conn, runId, startMs, termRunId, finished, background]
              (bool ok, const std::string &output, const std::string &error)
    {
        if (*finished)
            return;
        *finished = true;

        // ⚠ 映射这里**不删**: query 还要靠它把协议 runId 翻译成终端 runId 去复查历史。
        //   cancel 的安全性靠 activeRunId 校验(见 onLinkPacket), 不靠删映射。
        if (background)
        {
            // 后台: 启动时已回过 started, 这里补一条"结束"通知(不带 output —— 全文在终端
            // 运行记录里, 要就用 termQuery 取)。cli 靠它把 runId 从"在跑"集合里摘掉,
            // 否则一条自己退出的后台命令会让它永远显示忙。
            jzInfo().noquote().nospace() << "后台运行结束 " << runId
                                         << (ok ? " (ok)" : (" (失败: " + error + ")"));
        }

        JZNodePackExecResult res;
        res.runId = runId;
        res.ok = ok;
        res.durationMs = JZDateTime::currentMSecsSinceEpoch() - startMs;
        if (!background)
            res.output = cutExecOutput(output);
        res.error = error;
        if (m_linkTerminal)
        {
            res.cwd = m_linkTerminal->info().cwd;
            JZVirtualTerminal::RunRecordInfo info;
            if (!termRunId->empty() && m_linkTerminal->runSnapshot(*termRunId, info))
            {
                res.exitCode = info.exitCode;
                res.timedOut = info.timedOut;
                if (res.error.empty() && !info.error.empty())
                    res.error = info.error;
            }
        }
        sendLinkResult(conn, res);
    };

    JZResult<std::string> ret = m_linkTerminal->run(command, timeoutMs, cb);
    if (ret.isOk())
    {
        *termRunId = ret.value();
        m_linkTermRuns[key] = ret.value();
        pruneLinkRuns();

        if (background)
        {
            // 后台只是"这轮工具调用不阻塞": 立刻回 started, 结果等 cb 补一条。
            // run() 已经把命令投给执行线程, cb 不会在当前栈里执行, 所以这里发 started
            // 一定早于最终结果(顺序由主线程事件循环保证)。
            JZNodePackExecResult res;
            res.runId = runId;
            res.started = true;
            res.ok = true;
            res.cwd = m_linkTerminal->info().cwd;
            sendLinkResult(conn, res);
        }
    }
    else if (!*finished)
    {
        // 命令被拒/起不来(这种情况下 cb 通常已被同步调过一次, 用 finished 防重复回包)
        *finished = true;
        JZNodePackExecResult res;
        res.runId = runId;
        res.ok = false;
        res.error = ret.errorMessage();
        res.cwd = m_linkTerminal->info().cwd;
        sendLinkResult(conn, res);
    }
}

// 按 runId 取一次运行快照(等价本地的 terminal_query)。
// 过滤与游标推进都在节点侧做: 日志本来就在终端的运行记录里, 没必要整段搬过网络再让 cli 过滤。
void JZAgentNode::answerLinkQuery(uint64_t conn, const JZNodePackTermQuery &query)
{
    JZNodePackTermQueryResult res;
    res.runId = query.runId;

    // 打点: 现场排障时"查询到底有没有到节点"是最难猜的一件事(测试也靠这行断言)
    jzInfo().noquote().nospace() << "查询运行 "
                                 << (query.runId.empty() ? string("(最近一次)") : query.runId)
                                 << (query.incremental ? " (增量)" : "")
                                 << (query.grep.empty() ? "" : (" grep=" + query.grep));

    if (!m_linkTerminal)
    {
        res.found = false;
        res.error = "节点没有可用的执行终端";
        m_linkServer->sendPacket(conn, &res);
        return;
    }

    // runId 省略 = **这条连接**最近一次运行(与本地 terminal_query 同一口径)。
    // ⚠ 不能直接取 runIds().back(): 多台 cli 共享一台节点时, 那可能是别人的命令。
    std::string termRunId;
    if (query.runId.empty())
    {
        std::vector<std::string> ids = m_linkTerminal->runIds();
        for (size_t i = ids.size(); i > 0 && termRunId.empty(); --i)
        {
            for (auto &kv : m_linkTermRuns)
            {
                if (kv.first.first == conn && kv.second == ids[i - 1])
                {
                    termRunId = ids[i - 1];
                    break;
                }
            }
        }
    }
    else
    {
        auto it = m_linkTermRuns.find(linkKey(conn, query.runId));
        if (it != m_linkTermRuns.end())
            termRunId = it->second;
    }

    JZVirtualTerminal::RunRecordInfo info;
    if (termRunId.empty() || !m_linkTerminal->runSnapshot(termRunId, info))
    {
        res.found = false;
        res.error = query.runId.empty()
                        ? string("这个节点还没有运行过任何命令(没有可查的记录)")
                        : string("节点上没有这条运行记录(可能已结束很久 / 节点重启过 / run_id 不对)");
        m_linkServer->sendPacket(conn, &res);
        return;
    }

    // 回给模型的 runId 用**协议 id**(带节点前缀): 省略查询时模型也要能拿到它再去查下一次
    if (res.runId.empty())
    {
        for (auto &kv : m_linkTermRuns)
        {
            if (kv.first.first == conn && kv.second == termRunId)
            {
                res.runId = kv.first.second;
                break;
            }
        }
        if (res.runId.empty())
            res.runId = termRunId;   // 极端情况(映射被淘汰): 至少别回空串
    }

    res.found = true;
    res.running = (info.status == "running");
    res.ok = (!res.running && !info.timedOut && info.exitCode == 0);
    res.background = info.background;
    res.timedOut = info.timedOut;
    res.exitCode = info.exitCode;
    res.durationMs = (info.endTime > 0 && info.startTime > 0) ? (info.endTime - info.startTime) : 0;
    res.totalLines = (int64_t)info.totalLines;
    res.error = info.error;

    // 带 grep 的查询是"在整段日志里找东西"⇒ 按看历史处理, 不动增量游标(与本地一致),
    // 否则被过滤掉的行下一轮就永远读不到了。
    const bool incremental = query.incremental && query.grep.empty();

    // 绝对行号 = firstLine + 下标; 增量查询从上次游标之后开始
    size_t from = 0;
    if (incremental && info.readPos > info.firstLine)
        from = (size_t)(info.readPos - info.firstLine);
    if (from > info.log.size())
        from = info.log.size();

    std::vector<std::string> lines;
    for (size_t i = from; i < info.log.size(); ++i)
    {
        const std::string &line = info.log[i].text;
        if (!query.grep.empty() && !containsNoCase(line, query.grep))
            continue;
        lines.push_back(line);
    }
    size_t tailDropped = 0;
    if (query.tail > 0 && (int)lines.size() > query.tail)
    {
        tailDropped = lines.size() - (size_t)query.tail;
        lines.erase(lines.begin(), lines.end() - query.tail);
    }

    res.firstLine = (int64_t)(info.firstLine + from + tailDropped);

    // ⚠ 单包上限 256KB(MG_MAX_PACK_SIZE): 运行记录窗口本身可以有 4000 行 / 512KB,
    //   不裁就会把连接打成畸形包, 同时把模型上下文灌爆。这里按本地 terminal_query 的
    //   32KB 口径截断, 并明确告诉模型"还有多少行、怎么缩小范围"。
    const size_t kMaxQueryBytes = 32 * 1024;
    size_t emitted = 0;
    for (size_t i = 0; i < lines.size(); ++i)
    {
        const std::string one = lines[i] + "\n";
        if (res.text.size() + one.size() > kMaxQueryBytes)
        {
            if (emitted == 0)
            {
                // 单行就超过预算(压缩过的代码之类)。不推游标会永远卡在同一行,
                // 这里明确跳过并告知 —— 要原文就用 grep/tail 定位后单独处理。
                res.text += "…[本行超过 32KB, 已跳过; 要原文请用 grep/tail 缩小范围]";
                ++emitted;
            }
            else
            {
                res.text += "…[还有 " + std::to_string(lines.size() - i) +
                            " 行未显示: 用 tail 取最近 N 行, 或加 grep 缩小范围]";
            }
            break;
        }
        res.text += one;
        ++emitted;
    }
    while (!res.text.empty() && res.text.back() == '\n')
        res.text.pop_back();

    if (incremental)
    {
        // 游标只推进到**真正发出去的最后一行**, 不能用 totalLines:
        // 被 32KB 截断 / 被 tail 砍掉的行如果也跳过, 增量查询以后永远读不回来。
        const size_t sentPos = (size_t)(info.firstLine + from + tailDropped + emitted);
        m_linkTerminal->advanceRunReadPos(termRunId, sentPos);
        res.readPos = (int64_t)sentPos;
    }
    else
    {
        res.readPos = (int64_t)info.readPos;
    }

    m_linkServer->sendPacket(conn, &res);
}

// runId -> 终端 runId 的映射只增不减会把长期运行的节点撑大; 超上限先淘汰已结束的,
// 在跑的留着(否则 cancel/query 找不到)。
void JZAgentNode::pruneLinkRuns()
{
    static const size_t kMaxLinkRuns = 1024;
    if (m_linkTermRuns.size() <= kMaxLinkRuns)
        return;

    for (auto it = m_linkTermRuns.begin();
         it != m_linkTermRuns.end() && m_linkTermRuns.size() > kMaxLinkRuns;)
    {
        JZVirtualTerminal::RunRecordInfo info;
        if (m_linkTerminal && m_linkTerminal->runSnapshot(it->second, info) && info.status == "running")
        {
            ++it;
            continue;
        }
        it = m_linkTermRuns.erase(it);
    }
}
