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
        // 档位 = 预设: 先展开成字段, 再用显式配置的单项覆盖
        m_terminalConfig.applyProfile(m_config->profile);
        if (m_config->hasAllowPackageManager)
            m_terminalConfig.allowPackageManager = m_config->allowPackageManager;
        if (m_config->hasAllowNetwork)
            m_terminalConfig.allowNetwork = m_config->allowNetwork;
        if (m_config->hasAllowUnknownPrograms)
            m_terminalConfig.allowUnknownPrograms = m_config->allowUnknownPrograms;
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
        //   terminal_run    —— 单前台任务, 回包就是输出(没有 run_id / query)
        addTool(new JZTerminalRunMcpTool(this));
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
                                 << " verify-shell=" << (m_config.shellProgram.empty() ? string("auto(/bin/sh)") : m_config.shellProgram)
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
    text += "读写: 全盘可读(只读), 只有工作区/系统临时目录/标了可写的授信目录可写。\n";
    text += "命令策略(默认放行): " + string(m_config.allowPackageManager ? "允许" : "禁止") +
            "装/卸/升级依赖(pip/npm/apt...), " + string(m_config.allowNetwork ? "允许" : "禁止") +
            "联网下载(curl/wget...); 普通命令(含未登记的 ipconfig/uptime 之类)都能跑, "
            "只有破坏性命令(dd/sudo/shutdown...)与改系统设置的命令(reg/netsh/sc...)一律拒绝。\n\n";

    text += "可用工具:\n";
    if (m_config.allowTerminal)
    {
        text += "  terminal_run    执行一条命令(整条交给 shell, 一次一条)。\n";
        text += jzTerminalSessionNotes(true);
        if (m_config.maxSessions > 0)
        {
            text += "  - 会话上限 " + to_string(m_config.maxSessions) + " 个, 空闲 " +
                    to_string(m_config.recycleSec) + " 秒会被自动回收。\n";
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

    // 终端语义统一走 jzTerminalRunNotes(): 与本地 cli / remote_ssh / code_agent 同一份口径,
    // 别在这里另写一套(以前这里写死了"不支持后台 &"等旧说法, 和实际行为对不上)。
    text += jzTerminalRunNotes();
    // 节点特有: 每个会话是有状态解释器, cd/export 跨命令保留
    text += "  - 会话是有状态的: `cd` / `export` 保留到下一条命令; `cd X && <命令>` 之后相对路径按 X 解析,"
            "权限也按 X 判。\n";
    text += "  - 不确定当前目录就看结果末尾那行 `[cwd]`(单独的 `pwd` 也返回它)。\n";
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

    // link 模式: 终端按"连接 + session"懒创建(见 linkTerm), 生命周期跟着连接走 ——
    // 连接断(或被保活判死)就全部销毁, 没有空闲回收。
    // mcp 模式那套 terminal_open/close 是给模型自己管会话用的, remote_ssh 不需要。

    jzInfo().noquote().nospace() << "agent_node(link) 就绪: name=" << m_config.nodeName
                                 << " root=" << m_config.rootDir
                                 << " shell=jz_shell(内建)"
                                 << " 每连接终端上限=" << m_config.maxSessions
                                 << " 同时跑的命令上限=" << m_config.maxConcurrent
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
        dropLinkClient(conn);   // 终端生命周期 = 连接生命周期
    });
    startLinkWatchdog();

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
    // 连接已经没了(kill 之后收尾一定晚于断连): sendPacket 找不到 conn 会断言, 直接丢
    if (m_linkClients.find(conn) == m_linkClients.end())
        return;
    m_linkServer->sendPacket(conn, &result);
}

int JZAgentNode::linkRunningCount(const LinkClient &client) const
{
    int n = 0;
    for (const auto &kv : client.sessions)
        if (!kv.second.runId.empty())
            ++n;
    return n;
}

// 取(必要时懒建)某条连接名下的某台终端。session 为空 = 默认终端。
JZVirtualTerminal *JZAgentNode::linkTerm(uint64_t conn, const std::string &session, std::string &error)
{
    LinkClient &client = m_linkClients[conn];
    auto it = client.sessions.find(session);
    if (it != client.sessions.end())
        return it->second.term;

    // 每连接的终端数上限: 终端是有状态的 shell(cwd/export 各自独立), 不设上限会被
    // 一条连接一路开下去(每个都占一个执行线程)。
    if (m_config.maxSessions > 0 && (int)client.sessions.size() >= m_config.maxSessions)
    {
        error = "这条连接上的终端数已达上限(" + std::to_string(m_config.maxSessions) +
                "): 换一个 session 名, 或调大配置里的 maxSessions";
        return nullptr;
    }

    JZVirtualTerminal *t = new JZVirtualTerminal(this);
    t->setId(session.empty() ? std::string("link") : ("link-" + session));
    t->setTerminalConfig(m_host.toolTerminalConfig());
    // 回包截断由本文件按协议包上限做(cutExecOutput); 这里关掉终端那层 32KB 上下文阀,
    // 否则 120KB 的预算永远用不上。
    t->setReplyMaxBytes(0);

    // cwd 落在节点工作区根(空 = 进程 cwd; normalizeConfig 已把它补成绝对路径)
    std::string dirError;
    if (!t->changeDir(m_config.rootDir, dirError))
        jzWarning().noquote().nospace() << "link 模式设置工作目录失败: " << dirError;

    LinkSession s;
    s.term = t;
    client.sessions[session] = s;
    jzInfo().noquote().nospace() << "开终端 session=" << (session.empty() ? "(默认)" : session)
                                 << " conn=" << conn << " (这条连接共 "
                                 << client.sessions.size() << " 台)";
    return t;
}

// 连接断开(或被保活判死): 销毁它名下全部终端 —— 析构会 kill 在跑的进程。
// 这正是 shell 的语义: 终端没了, 前台任务跟着死, 不做断线续跑。
void JZAgentNode::dropLinkClient(uint64_t conn)
{
    auto it = m_linkClients.find(conn);
    if (it == m_linkClients.end())
        return;

    for (auto &kv : it->second.sessions)
    {
        if (!kv.second.runId.empty())
            jzInfo().noquote().nospace() << "停掉掉线 cli 的命令: " << kv.second.runId
                                         << " (session=" << (kv.first.empty() ? "默认" : kv.first) << ")";
        delete kv.second.term;   // 析构里 cancel + 等执行线程退出
    }
    m_linkClients.erase(it);
}

void JZAgentNode::startLinkWatchdog()
{
    if (m_linkWatchdog)
        return;
    m_linkWatchdog = new JZTimer(this);
    connect(&m_linkWatchdog->sigTimeout, this, [this]() { checkLinkAlive(); });
    m_linkWatchdog->start(5000);   // 与 cli 的 ping 周期同频, 判定阈值 20s(4 次没到)
    jzInfo().noquote().nospace() << "保活看门狗已启动(5s 扫描 / 20s 判定)";
}

void JZAgentNode::checkLinkAlive()
{
    if (!m_linkServer || m_linkClients.empty())
        return;

    const int64_t now = JZDateTime::currentMSecsSinceEpoch();
    std::vector<uint64_t> dead;
    for (const auto &kv : m_linkClients)
        if (kv.second.lastPingMs > 0 && now - kv.second.lastPingMs > 20000)
            dead.push_back(kv.first);

    for (uint64_t conn : dead)
    {
        // 与"真断开"同一条收口路径: 销毁终端(kill 在跑的命令), 再关连接(对面会走退避重连)
        jzWarning().noquote().nospace() << "cli 静默超时(>20s), 按掉线处理: conn=" << conn;
        dropLinkClient(conn);
        m_linkServer->closeClient(conn);
    }
}

void JZAgentNode::onLinkPacket(uint64_t conn, JZNetPackPtr pack)
{
    if (!pack)
        return;

    const int type = pack->packetType();

    // 任何包都说明这条链路活着; 只有"发过 ping"的连接才进超时判定(老 cli 不发 ping)
    const int64_t nowMs = JZDateTime::currentMSecsSinceEpoch();
    LinkClient &client = m_linkClients[conn];
    if (client.lastPingMs > 0)
        client.lastPingMs = nowMs;

    if (type == NodePack_ping)
    {
        const bool firstPing = (client.lastPingMs == 0);
        client.lastPingMs = nowMs;   // 这条连接会保活: 从下一次扫描起参与超时判定
        if (firstPing)
            jzInfo().noquote().nospace() << "cli 开始保活(ping): conn=" << conn;
        JZNodePackPong pong;
        pong.atMs = nowMs;
        m_linkServer->sendPacket(conn, &pong);
        return;
    }

    if (type == NodePack_pong)
        return;   // 节点不发 ping, 收到 pong 只可能是对面回显: 忽略

    if (type == NodePack_hello)
    {
        JZNodePackHello *hello = (JZNodePackHello *)pack.get();
        JZNodePackHelloAck ack;
        // 终端是懒创建的(session 由 cli 在 exec 里指定), 这里只报能力
        ack.ok = m_config.allowTerminal;
        ack.error = ack.ok ? std::string() : std::string("节点没开终端(allowTerminal=false)");
        ack.nodeName = m_config.nodeName;
        ack.version = m_config.version;
        ack.rootDir = m_config.rootDir;
        // 终端用内建解释器, 没有可配的外部 shell; 这个字段保留给 cli 展示。
        ack.shell = m_config.allowTerminal ? std::string("jz_shell") : std::string();
        m_linkServer->sendPacket(conn, &ack);
        jzInfo().noquote().nospace() << "cli 上线: id=" << hello->clientId << " v" << hello->cliVersion
                                     << " conn=" << conn;
        return;
    }

    if (type == NodePack_exec)
    {
        JZNodePackExec *exec = (JZNodePackExec *)pack.get();
        runLinkExec(conn, exec->session, exec->runId, exec->command, exec->cwd,
                    exec->timeoutMs, exec->background);
        return;
    }

    if (type == NodePack_termQuery)
    {
        // 协议包保留(旧版 cli 还会发), 但终端已经没有"运行记录"可查 —— 明确回一句,
        // 免得对面等 10s 超时还以为是网络问题。
        JZNodePackTermQuery *query = (JZNodePackTermQuery *)pack.get();
        JZNodePackTermQueryResult res;
        res.runId = query->runId;
        res.found = false;
        res.error = "节点不保留运行记录(没有 run_id/terminal_query); 后台任务请写成 `> log 2>&1 &`, 再看日志文件";
        m_linkServer->sendPacket(conn, &res);
        return;
    }

    if (type == NodePack_sess)
    {
        JZNodePackSess *req = (JZNodePackSess *)pack.get();
        JZNodePackSessResult res;
        res.reqId = req->reqId;
        auto cit = m_linkClients.find(conn);
        const bool haveClient = (cit != m_linkClients.end());

        if (!m_config.allowTerminal)
            res.error = "节点没开终端(allowTerminal=false)";
        else if (req->op == "open")
        {
            // 名字省略 = 节点起一个; 同名已存在则复用(幂等, 不重建 —— 重建会把在跑的命令杀掉)
            const std::string name = req->session.empty() ? ("s" + std::to_string(++m_linkSessionGen))
                                                         : req->session;
            const bool existed = haveClient &&
                cit->second.sessions.find(name) != cit->second.sessions.end();
            std::string err;
            JZVirtualTerminal *t = linkTerm(conn, name, err);
            if (!t)
                res.error = err;
            else if (!req->cwd.empty() && !t->changeDir(req->cwd, err))
                res.error = "cwd 不可用: " + err;
            else
            {
                res.ok = true;
                res.session = name;
                res.text = (existed ? "会话已存在(复用)" : "已开终端") + std::string(" session=") + name +
                           " cwd=" + t->info().cwd +
                           "\n之后用 session=\"" + name + "\" 在这台终端上跑命令(它的 cwd/export 独立保留)";
            }
        }
        else if (req->op == "close")
        {
            auto sit = haveClient ? cit->second.sessions.find(req->session) : cit->second.sessions.end();
            if (sit == cit->second.sessions.end())
                res.error = "没有这个会话: " + req->session;
            else
            {
                JZVirtualTerminal *t = sit->second.term;
                const std::string running = sit->second.runId;
                cit->second.sessions.erase(sit);
                delete t;   // 析构会 kill 在跑的命令
                res.ok = true;
                res.session = req->session;
                res.text = "已关闭 session=" + req->session +
                           (running.empty() ? std::string() : ("(在跑的命令已终止: " + running + ")"));
            }
        }
        else if (req->op == "list")
        {
            res.ok = true;
            std::string text = "会话数: ";
            const int n = haveClient ? (int)cit->second.sessions.size() : 0;
            text += std::to_string(n);
            if (n == 0)
                text += "\n(这条连接还没有终端: run 时会自动建一个, 或先 open 命名)";
            else
            {
                for (const auto &kv : cit->second.sessions)
                {
                    text += "\n  - ";
                    text += kv.first.empty() ? std::string("(默认)") : kv.first;
                    text += "  cwd=" + kv.second.term->info().cwd;
                    text += kv.second.runId.empty() ? std::string("  空闲") : ("  在跑 " + kv.second.runId);
                }
            }
            res.text = text;
        }
        else
            res.error = "未知的会话操作: " + req->op;

        m_linkServer->sendPacket(conn, &res);
        return;
    }

    if (type == NodePack_execCancel)
    {
        JZNodePackExecCancel *cancel = (JZNodePackExecCancel *)pack.get();
        auto cit = m_linkClients.find(conn);
        if (cit == m_linkClients.end())
            return;
        auto sit = cit->second.sessions.find(cancel->session);
        if (sit == cit->second.sessions.end() || sit->second.runId != cancel->runId)
        {
            // 已结束(或不是这个会话在跑的那条): 明确忽略。**绝不能**直接 cancel():
            // 那时在跑的很可能是同会话后来的命令。
            jzInfo().noquote().nospace() << "取消: " << cancel->runId
                                         << " 不是该会话在跑的那条, 忽略";
            return;
        }

        jzInfo().noquote().nospace() << "取消执行: " << cancel->runId
                                     << " (session=" << (cancel->session.empty() ? "默认" : cancel->session) << ")";
        sit->second.term->cancel();
        return;
    }
}

void JZAgentNode::runLinkExec(uint64_t conn, const std::string &session, const std::string &runId,
                              const std::string &command, const std::string &cwd, int timeoutMs,
                              bool background)
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

    // 同一会话(终端)同时只能跑一条命令: 忙就直接失败, 不排队 —— 排队会让调用方
    // 以为"已经在跑了", 而 cwd 还停在上一条中途。
    auto cit = m_linkClients.find(conn);
    if (cit != m_linkClients.end())
    {
        auto sit = cit->second.sessions.find(session);
        if (sit != cit->second.sessions.end() && sit->second.term->isRunning())
        {
            fail(session.empty() ? std::string("节点忙: 默认终端上一条命令还在跑, 等它结束再来")
                                 : ("节点忙: 会话 " + session + " 上一条命令还在跑, 等它结束再来"));
            return;
        }
        // 整个连接的并发上限(maxConcurrent): 不同 session 之间也限一下, 免得一台设备被跑满
        if (m_config.maxConcurrent > 0 && linkRunningCount(cit->second) >= m_config.maxConcurrent)
        {
            fail("节点忙: 这条连接已有 " + std::to_string(m_config.maxConcurrent) + " 条命令在跑");
            return;
        }
    }

    std::string termError;
    JZVirtualTerminal *term = linkTerm(conn, session, termError);
    if (!term)
    {
        fail("没有可用的执行终端: " + termError);
        return;
    }

    // cwd: 空 = 会话当前目录(工作区根); 非空直接换目录 ——
    // 以前是拼 `cd "<cwd>" && <命令>`, 只拒双引号, `$()`/反引号照样会被 shell 展开,
    // 等于给 cwd 留了个绕过命令审核的口子。changeDir 自己会审权限, 也不经过 shell 解析。
    if (!cwd.empty())
    {
        std::string dirError;
        if (!term->changeDir(cwd, dirError))
        {
            fail("cwd 不可用: " + dirError);
            return;
        }
    }

    const int64_t startMs = JZDateTime::currentMSecsSinceEpoch();
    auto finished = std::make_shared<bool>(false);

    // ⚠ 不捕获 term: 连接断开时终端会被销毁, 回调可能晚一步到, 拿指针就是野指针。
    //   需要 cwd 就从"连接/会话还在不在"里查。
    auto cb = [this, conn, session, runId, startMs, finished, background]
              (bool ok, const std::string &output, const std::string &error)
    {
        if (*finished)
            return;
        *finished = true;

        // 这条跑完了: 若归属还是它, 清空(cancel 才能正确判定"没有在跑的")
        std::string cwdNow;
        auto ci = m_linkClients.find(conn);
        if (ci != m_linkClients.end())
        {
            auto si = ci->second.sessions.find(session);
            if (si != ci->second.sessions.end())
            {
                if (si->second.runId == runId)
                    si->second.runId.clear();
                if (si->second.term)
                    cwdNow = si->second.term->info().cwd;
            }
        }

        if (background)
        {
            // 后台: 启动时已回过 started, 这里补一条"结束"通知(不带 output ——
            // 输出在命令自己的重定向文件里)。cli 靠它把 runId 从"在跑"集合里摘掉,
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
        res.cwd = cwdNow;
        sendLinkResult(conn, res);
    };

    JZResult<void> ret = term->run(command, timeoutMs, cb);
    if (ret.isOk())
    {
        // cancel 归属: 只认"这条会话当前在跑的 runId"
        m_linkClients[conn].sessions[session].runId = runId;

        if (background)
        {
            // 后台只是"这轮工具调用不阻塞": 立刻回 started, 结果等 cb 补一条。
            // run() 已经把命令投给执行线程, cb 不会在当前栈里执行, 所以这里发 started
            // 一定早于最终结果(顺序由主线程事件循环保证)。
            JZNodePackExecResult res;
            res.runId = runId;
            res.started = true;
            res.ok = true;
            res.cwd = term->info().cwd;
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
        res.cwd = term->info().cwd;
        sendLinkResult(conn, res);
    }
}


