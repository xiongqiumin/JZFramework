#include "JZAgentNode.h"

#include "JZByteArray.h"
#include "JZDebug.h"
#include "JZDir.h"
#include "JZFile.h"
#include "JZFileInfo.h"
#include "JZMcpCore.h"
#include "JZMcpServer.h"
#include "JZMcpToolBridge.h"
#include "JZTerminalTools.h"
#include "JZTerminalMcpTools.h"
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
    m_zone.workspace = m_config ? m_config->rootDir : string();
    m_zone.trustedDirs = m_config ? m_config->trustedDirs : vector<TrustedDir>();
    // 节点侧的命令策略: 没有"安装/下载"这类对话式放行, 一律按默认拒绝
    // (真要装东西, 让模型显式用 terminal_run 跑一条, 由人看着)。
    m_policy = ProgramPolicy();
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

    // ---- 终端: 这台机器上唯一的执行面与审核面 ----
    // 文件操作不再是单独的工具了 —— ls / cat / mkdir / rm / cp / mv / grep 都是 shell
    // 命令, 由 terminal_run 直接覆盖; 只有 edit_file 留下来(sed 不校验唯一匹配)。
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
        // 文件读写走 agent_tool 里现成的 read_file / write_file / modify_file
        // (sed 不校验唯一匹配, 且 modify_file 回上下文片段; 大文件读取用 read_file 分段)
        addTool(new JZReadFileTool());
        addTool(new JZWriteFileTool());
        addTool(new JZModifyFileTool());
    }
    else
    {
        jzWarning() << "allowTerminal=false: 这个节点一个工具都没注册";
    }

    jzInfo().nospace() << "agent_node 就绪: name=" << m_config.nodeName
                       << " root=" << m_config.rootDir
                       << " shell=" << (m_config.shellProgram.empty() ? string("auto(/bin/sh)") : m_config.shellProgram);
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

string JZAgentNode::buildInstructions() const
{
    string text;
    text += "你是运行在主机 \"" + m_config.nodeName + "\" 上的操作节点。\n";
    text += "工作区: " + m_config.rootDir + "  —— 相对路径以它为基准; "
            "越出工作区与授信目录的路径会被拒绝。\n\n";

    text += "可用工具:\n";
    text += "  terminal_run    执行一条命令。命令**整条交给 shell**, "
            "管道(|)、重定向(> >>)、串联(&& || ;)、通配符(*)、变量($VAR) 都按 shell 的语义工作。\n";
    text += "  modify_file     精确字符串替换(要求唯一匹配); old_str 留空 = 整文件覆盖(新建/重写)。"
            "改代码用这个, **不要用 sed**。\n";
    text += "  terminal_open / terminal_list / terminal_close   多会话管理。⚠ terminal_run "
            "**必须先 terminal_open** 拿到 session id 才能跑(节点是单租户, 不再自动开默认会话)。\n";
    text += "  terminal_query   取后台/历史运行的日志(见下方 background / 流式说明)。\n\n";

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
    text += "  - 只有**单独一条** `cd <目录>` 会改变后续命令的目录。"
            "`cd build && cmake ..` 里的 cd 只在那一句内有效。\n";
    text += "    terminal_run 的结果末尾会回一行 `[cwd]`, 那是你当前真正的目录 —— 不确定就看它。\n";
    text += "  - 环境变量不跨命令保留, 要用就内联写: `FOO=bar <命令>`。\n";
    text += "  - 不支持: 后台 `&`、nohup/setsid/screen、`$( )`、反引号、heredoc、进程替换。\n";
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

std::string JZAgentNode::open(const std::string &cwd, std::string &error)
{
    // 每个会话一个独立终端实例(各自 cwd / 前后台进程 / 运行记录);
    // JZVirtualTerminal 本身不含会话概念, 生命周期由本节点管。
    JZVirtualTerminal *t = new JZVirtualTerminal(this);
    t->setZoneConfig(m_host.toolZone());
    t->setProgramPolicy(m_host.toolProgramPolicy());
    if (!m_config.shellProgram.empty())
        t->setShellProgram(m_config.shellProgram);
    if (m_config.recycleSec > 0)
        t->setRecycleMs(m_config.recycleSec * 1000);

    if (!cwd.empty() && !t->changeDir(cwd, error))
        return std::string();   // changeDir 已填 error(越界等)

    std::string sid = "t" + std::to_string(++m_sessionIdGen);
    t->setId(sid);
    m_terminals[sid] = t;
    return sid;
}

bool JZAgentNode::close(const std::string &sessionId)
{
    auto it = m_terminals.find(sessionId);
    if (it == m_terminals.end())
        return false;
    JZVirtualTerminal *t = it->second;
    m_terminals.erase(it);
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
