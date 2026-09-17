#ifndef JZ_AGENT_NODE_H_
#define JZ_AGENT_NODE_H_

#include <map>
#include <string>
#include <vector>

#include "JZCodeTools.h"     // ICodeToolHost / ZoneConfig / ProgramPolicy / TrustedDir
#include "JZObject.h"
#include "JZTerminalRegistry.h"   // JZTerminalRegistry: 会话 -> 终端 的注册表(本节点实现)

class JZMcpServer;
class JZAgentTool;
class JZMcpTool;           // 直接挂进 MCP 的工具(不经过 agent_tool 包装)
class JZVirtualTerminal;   // 执行/审核面, 见 agent_tool/JZVirtualTerminal.h(目前只有声明, 待接)

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

    // 文件工具的根目录(相对路径按它拼接), 同时也是区域模型里的"工作区"(可读写)。
    // 为空时取进程当前工作目录。
    std::string rootDir;

    // 授信目录(编译器/SDK 之类), 默认只读, 需要写就设 writable。
    std::vector<TrustedDir> trustedDirs;

    // 是否注册终端工具。⚠ 关掉的话这个节点就**一个工具都没有了** ——
    // 终端是这台机器上唯一的执行面(文件操作全走 shell 命令), 所以一般保持开着。
    bool allowTerminal = true;

    // 跑命令用哪个 shell。空 = 自动探测: POSIX 是 /bin/sh;
    // Windows 是 Git for Windows 的 Git/bin/bash.exe(探测不到会直接报错, 不退回 cmd)。
    std::string shellProgram;

    // 会话空闲多久回收(秒), <=0 表示不回收。默认 15 分钟。
    int recycleSec = 15 * 60;

    // HTTP 监听端口(端点 /mcp)。默认 39000。
    int port = 39000;

    std::string logFile;         // 预留: 日志落文件(stdio 模式用)
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

    const ZoneConfig &toolZone() const override { return m_zone; }
    const ProgramPolicy &toolProgramPolicy() const override { return m_policy; }
    bool toolReadFile(const std::string &absPath, std::string &text, std::string &error) override;

private:
    const JZNodeConfig *m_config = nullptr;
    ZoneConfig m_zone;
    ProgramPolicy m_policy;
};

// ---------------------------------------------------------------------------
// JZAgentNode —— 被控端节点: 把本机能力包成一个 MCP server
//
// 传输方式(HTTP / 后面要加的 stdio)由 main.cpp 决定, 本类只负责
// "这台机器上有什么工具":
//   终端(MCP 直出门面):  terminal_run / terminal_query / terminal_open / terminal_list / terminal_close
//   编辑:  modify_file(精确替换 + 唯一匹配校验 + 回上下文片段)
// 文件操作(ls/cat/mkdir/rm/cp/mv/grep...)不再单独做工具 —— 它们都是 shell 命令,
// 由 terminal_run 覆盖; 只有 modify_file 留在我们这边(sed 不校验唯一匹配)。
// 终端 MCP 工具直接继承 JZMcpTool(terminal 的两个门面之一); 其余工具经 JZMcpToolBridge 挂成 MCP。
// ---------------------------------------------------------------------------
class JZAgentNode : public JZObject, public JZTerminalRegistry
{
public:
    explicit JZAgentNode(JZObject *parent = nullptr);
    ~JZAgentNode() override;

    // 建 server、注册工具、填 serverInfo。必须在 listen 之前调用。
    void init(const JZNodeConfig &config);

    JZMcpServer *server() const { return m_server; }
    const JZNodeConfig &config() const { return m_config; }
    JZTerminalRegistry *terminalRegistry() { return this; }

    // ---- JZTerminalRegistry: 会话 -> 终端 ----
    JZVirtualTerminal *terminal(const std::string &sessionId, std::string &error) override;
    std::string open(const std::string &cwd, std::string &error) override;
    bool close(const std::string &sessionId) override;
    std::vector<JZVirtualTerminal::SessionInfo> list() override;
    bool runSnapshot(const std::string &runId, JZVirtualTerminal::RunRecordInfo &out) const override;

protected:
    // 把 agent_tool 的一个工具经 bridge 挂进 MCP(接管 tool 所有权)
    void addTool(JZAgentTool *tool);
    // 把 JZMcpTool 直接挂进 MCP(接管 tool 所有权)—— 如 terminal 的 MCP 门面
    void addTool(JZMcpTool *tool);

    std::string buildInstructions() const;

    JZNodeConfig m_config;
    JZAgentNodeHost m_host;
    JZMcpServer *m_server = nullptr;

    // 会话 -> 终端。每个会话一个独立 JZVirtualTerminal(各自 cwd / 前后台进程 / 运行记录);
    // 生命周期(创建/销毁/空闲回收)由节点管。终端是 this 的子对象(随节点销毁),
    // MCP 工具只持注册表、不绑定具体终端。
    std::map<std::string, JZVirtualTerminal *> m_terminals;
    int m_sessionIdGen = 0;
};

#endif // !JZ_AGENT_NODE_H_
