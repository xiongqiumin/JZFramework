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
#include "JZTerminalTools.h"
#include "JZTerminalMcpTools.h"
#include "JZTimer.h"
#include "JZVirtualTerminal.h"

using namespace std;

// 单次读文件的上限。工具自己还会按行裁剪, 这里只是避免把几百兆的东西读进内存。
static const int kReadFileLimit = 8 * 1024 * 1024;

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

void JZAgentNode::init(const JZNodeConfig &config)
{
    m_config = config;

    if (m_config.nodeName.empty())
        m_config.nodeName = "agent_node";
    if (m_config.rootDir.empty())
        m_config.rootDir = JZDir::currentPath();

    // 统一成 "/" 分隔的绝对路径: 区域判定是纯字符串前缀比较, 不规范化会对不上
    m_config.rootDir = codeNormalizePath(m_config.rootDir);
    if (m_config.rootDir.empty())
        m_config.rootDir = codeNormalizePath(JZDir::currentPath());

    for (TrustedDir &dir : m_config.trustedDirs)
        dir.path = codeNormalizePath(dir.path);

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

    // ⚠ 这段是这个节点最重要的一段说明: 模型的 shell 直觉在这里只有一部分成立,
    // 不写清楚它一定会踩(cd 不持久 / export 不持久 / & 不支持)。
    text += "⚠ 这里**没有常驻 shell**, 每条命令都是一个新进程:\n";
    text += "  - `cd` 只对**当前这一条命令**有效: 要进目录执行就写 `cd <目录> && <命令>`。"
            "单独一条 `cd` 会被拒绝(它改变不了后面的命令)。\n";
    text += "    `cd X &&` 后面的相对路径按 X 解析, 权限也按 X 判 —— "
            "cd 到工作区外(临时目录除外)依然写不了。\n";
    text += "    相对路径一律以工作区根为基准: terminal_run 结果末尾那行 `[cwd]` 就是它,"
            "不确定就看它(单独的 `pwd` 也返回它)。\n";
    text += "  - 环境变量不跨命令保留, 要用就内联写: `FOO=bar <命令>`。\n";
    text += "  - 不支持: 后台 `&`、nohup/setsid/screen、`$( )`、反引号、heredoc、进程替换, "
            "以及 for/while/if 等复合语句(内部命令审不到)。\n";
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
