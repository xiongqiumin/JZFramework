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
    m_terminal = new JZVirtualTerminal(this);
    m_terminal->setZoneConfig(m_host.toolZone());
    m_terminal->setProgramPolicy(m_host.toolProgramPolicy());
    if (!m_config.shellProgram.empty())
        m_terminal->setShellProgram(m_config.shellProgram);
    if (m_config.recycleSec > 0)
        m_terminal->setRecycleMs(m_config.recycleSec * 1000);

    if (m_config.allowTerminal)
    {
        addTool(new JZTerminalRunTool(m_terminal));
        addTool(new JZTerminalOpenTool(m_terminal));
        addTool(new JZTerminalListTool(m_terminal));
        addTool(new JZTerminalCloseTool(m_terminal));
        // 文件编辑走 agent_tool 里现成的 modify_file(sed 不校验唯一匹配, 且它回上下文片段)
        addTool(new JZModifyFileTool());
    }
    else
    {
        jzWarning() << "allowTerminal=false: 这个节点一个工具都没注册";
    }

    jzInfo().nospace() << "agent_node 就绪: name=" << m_config.nodeName
                       << " root=" << m_config.rootDir
                       << " shell=" << m_terminal->shellProgram();
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
    text += "  terminal_open / terminal_list / terminal_close   多会话(一般用不到, "
            "terminal_run 会自动开一个默认会话)。\n\n";

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
