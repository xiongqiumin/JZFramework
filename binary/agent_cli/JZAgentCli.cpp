#include "JZAgentCli.h"

#include "JZByteArray.h"
#include "JZDebug.h"
#include "JZDir.h"
#include "JZFile.h"
#include "JZFileInfo.h"
#include "JZNetworkAccessManager.h"   // 联网工具共享的访问管理器(+ JZProxyConfig)
#include "JZPlatform.h"
#include "JZProcess.h"       // 验证命令的异步执行(超时/进程树收尾)
#include "JZString.h"        // str_toLower
#include "JZVirtualTerminal.h"
#include "json/json.h"
#include "JZLog.h"              // JZLogManager / LogModule_Agent / LOG_INFO

#include <string>

// 回合诊断日志: 把工具调用 / 思考 / 回复 / 输入 记进独立的 Agent 日志模块(模块 id = LogModule_Agent)。
// 该模块不走 Console, 只有配置 logFile 才落盘到 <logFile目录>/Agent.log, 没配就不打印。
// kMaxRoundLogLen 是单条记录上限, 避免长输出(命令回显 / 大段代码)把日志刷爆。
namespace {
const int kMaxRoundLogLen = 2000;   // 单条记录上限(字符), 超长截断并标注剩余长度
std::string logTrunc(const std::string &s)
{
    if ((int)s.size() <= kMaxRoundLogLen)
        return s;
    return s.substr(0, kMaxRoundLogLen)
           + "...[trunc " + std::to_string(s.size() - kMaxRoundLogLen) + "]";
}

// Agent 模块日志。JZLog 多模块: 每个模块独立 type(Console/File/None), 互不干扰。
static void agentLog(const std::string &msg)
{
    JZLogManager::instance()->log(LogModule_Agent, LOG_INFO, msg);
}

// think/reply 日志: 每单元一条(一条 think / 一条 reply / 一条 tool_call / 一条 tool_result), 超长折叠头尾
const size_t kMaxLine = 1200;   // 单条日志最大字节, 超出给总字节数 + 头 + 尾
const size_t kHead = 500, kTail = 300;

// 把一段待写缓冲格式化为单条日志: 太长则截断为"总字节数 + 头部 + 尾部"
static std::string logChunk(const char *tag, const std::string &pending, size_t totalBytes)
{
    if (pending.size() <= kMaxLine)
        return std::string(tag) + ": " + pending;
    size_t skip = pending.size() - kHead - kTail;
    return std::string(tag) + "[总 " + std::to_string(totalBytes) + " 字节, 本段 "
         + std::to_string(pending.size()) + "]: " + pending.substr(0, kHead)
         + "\n  …(省略 " + std::to_string(skip) + " 字节)…\n"
         + pending.substr(pending.size() - kTail);
}
}

#include "JZHttpServer.h"     // HTTP 服务(路由 / JZHttpServerClient / JZHttpRespone)
#include "JZHttpHandler.h"
#include "JZHttpStaticFiles.h"

#include "JZFrpClient.h"      // 内嵌 FRP 客户端(frpc, client 角色)
#include "JZNet/JZNetPacket.h"  // JZNetPackInit(frpc 的 JZNetClient 依赖包注册)

#include <cstdio>
#include <cstdlib>            // atoi(解析 "HTTP 402 ..." 里的状态码)
#ifndef _WIN32
#include <sys/wait.h>         // WIFEXITED / WEXITSTATUS(popen 取退出码)
#endif

#include "JZTimer.h"          // 重试前的延时(JZTimer::singleShot)

#include "JZTerminalTools.h"  // 本地终端工具(JZTerminalRunTool 等)
#include "JZMcpClient.h"      // MCP 客户端(连外部 MCP 服务器, 把远端工具挂进本 agent)

#include <iostream>
#include <fstream>            // system.extraFile(宿主常驻说明)读文件
#include <sstream>
#include <thread>
#include <chrono>             // 历史记录的起止时间戳(nowMs)

using namespace std;

const int JZAgentCli::kHistoryMax;

// 单次读文件上限(与 node 一致): 避免把几百兆读进内存。
static const int kReadFileLimit = 8 * 1024 * 1024;

// 验证输出回包上限(与旧 popen 实现一致): 几十 KB 足够定位, 再大只会撑爆 JSON 回包与上下文。
static const size_t kMaxVerifyBytes = 65536;

// /task 的图片附件上限(张数 / 单张字符数 / 合计字符数)。
//   张数与 code_agent 输入框的附图上限对齐; 字符上限按"截图 base64 通常几百 KB"给足余量,
//   但必须有硬上限 —— 整个 body 都要进内存, 还要再塞进 LLM 请求体。
//   ⚠ 合计上限必须明显小于 main.cpp 里抬高的 net maxRecvSize(32MB), 否则先被 mongoose
//     的接收缓冲上限掐断(客户端只看到 connection aborted, 没有任何错误回包)。
static const int kMaxTaskImages = 8;
static const int kMaxImageChars = 8 * 1024 * 1024;
static const int kMaxImagesTotalChars = 24 * 1024 * 1024;

// JZByteArray -> std::string(进程输出按字节接回来, 原样存)
static std::string bytesToText(const JZByteArray &data)
{
    if (data.size() <= 0 || !data.data())
        return std::string();
    return std::string(data.data(), data.size());
}

// 临时目录(放验证命令的包装脚本用): TEMP/TMPDIR 都没有就退回当前目录。
static std::string tempDirPath()
{
#ifdef _WIN32
    const char *t = std::getenv("TEMP");
    if (!t || !*t)
        t = std::getenv("TMP");
#else
    const char *t = std::getenv("TMPDIR");
#endif
    std::string dir = (t && *t) ? std::string(t) : std::string(".");
    for (size_t i = 0; i < dir.size(); i++)
    {
        if (dir[i] == '\\')
            dir[i] = '/';
    }
    while (dir.size() > 1 && dir.back() == '/')
        dir.pop_back();
    return dir;
}

// ==========================================================================
// JZCliToolHost
// ==========================================================================

JZCliToolHost::JZCliToolHost(const JZCliConfig *config)
    : m_config(config)
{
}

void JZCliToolHost::refresh()
{
    m_terminalConfig.zone.workspace = m_config ? m_config->rootDir : string();
    m_terminalConfig.zone.trustedDirs = m_config ? m_config->trustedDirs : vector<TrustedDir>();
    if (m_config)
    {
        m_terminalConfig.level = m_config->unrestricted ? AccessLevel::Unrestricted
                                                        : AccessLevel::Standard;
        m_terminalConfig.allowPackageManager = m_config->allowPackageManager;
        m_terminalConfig.allowNetwork = m_config->allowNetwork;
        m_terminalConfig.programPaths = m_config->programPaths;
    }
}

bool JZCliToolHost::toolReadFile(const std::string &absPath, std::string &text, std::string &error)
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
// JZAgentCli
// ==========================================================================

JZAgentCli::JZAgentCli(JZObject *parent)
    : JZObject(parent)
    , m_host(&m_config)
{
}

JZAgentCli::~JZAgentCli()
{
}

bool JZAgentCli::init(const JZCliConfig &config, std::string &error)
{
    m_config = config;

    // 程序映射: 只用配置里给的(外部传入)。这里不做任何自动探测 —— 没配就是没得用。
    if (m_config.rootDir.empty())
        m_config.rootDir = JZDir::currentPath();
    m_config.rootDir = codeNormalizePath(m_config.rootDir);
    if (m_config.rootDir.empty())
        m_config.rootDir = codeNormalizePath(JZDir::currentPath());
    for (TrustedDir &dir : m_config.trustedDirs)
        dir.path = codeNormalizePath(dir.path);

    m_host.refresh();

    m_agent = new JZAgent(this);
    m_agent->setStreamEnabled(true);
    m_agent->setThinkEnabled(true);

    JZAgentConfig ac;
    ac.rootPath = m_config.rootDir;
    ac.modelUrl = m_config.modelUrl;
    ac.modelName = m_config.modelName;
    ac.modelToken = m_config.modelToken;
    ac.netTimeout = m_config.netTimeout;
    ac.maxRounds = m_config.maxRounds;
    ac.maxToken = m_config.maxToken;
    ac.extraBody = m_config.extraBody;
    ac.reuseConnection = false;
    ac.tokenStrategy = JZAgentConfig::TokenCompress;
    // 循环检测(工具重复调用 / 思考回复原地打转): 命中直接中止本轮(见 JZCliConfig 同名字段)
    ac.loopDetect.enabled = m_config.loopDetect;
    ac.loopDetect.window = m_config.loopWindow;
    ac.loopDetect.repeat = m_config.loopRepeat;
    ac.loopDetect.sameResult = m_config.loopSameResult;
    ac.loopDetect.pollRepeat = m_config.loopPollRepeat;
    ac.loopDetect.pollSameResult = m_config.loopPollSameResult;
    ac.loopDetect.textLineRepeat = m_config.loopLineRepeat;
    ac.loopDetect.textBlockRepeat = m_config.loopBlockRepeat;
    ac.loopDetect.textPeriodMax = m_config.loopPeriodMax;
    ac.loopDetect.textPeriodRepeat = m_config.loopPeriodRepeat;
    m_agent->init(ac);

    connect(&m_agent->sigReply, this, &JZAgentCli::onReply);
    connect(&m_agent->sigThinking, this, &JZAgentCli::onThinking);
    connect(&m_agent->sigToolCall, this, &JZAgentCli::onToolCall);
    connect(&m_agent->sigToolResult, this, &JZAgentCli::onToolResult);
    connect(&m_agent->sigError, this, &JZAgentCli::onError);
    connect(&m_agent->sigFinished, this, &JZAgentCli::onFinished);

    setupAgent();
    const string sysPrompt = buildSystemPrompt();
    m_agent->setSystemPrompt(sysPrompt);
    m_systemPromptBytes = (int)sysPrompt.size();   // 记录一份给 GET /info(核对"常驻说明有没有生效")
    jzInfo() << "system prompt: " << sysPrompt.size() << " bytes"
             << ((m_config.systemExtra.empty() && m_config.systemExtraFile.empty())
                     ? "" : "（含宿主常驻说明 system.extra/extraFile）");
    return true;
}

void JZAgentCli::setupAgent()
{
    m_terminal = new JZVirtualTerminal(this);
    m_terminal->setId("default");
    m_terminal->setTerminalConfig(m_host.toolTerminalConfig());
    if (!m_config.shellProgram.empty())
        m_terminal->setShellProgram(m_config.shellProgram);
    if (m_config.recycleSec > 0)
        m_terminal->setRecycleMs(m_config.recycleSec * 1000);

    auto addTool = [this](JZCodeTool *tool)
    {
        tool->setHost(&m_host);
        m_agent->addTool(tool);
        m_tools.push_back(tool);
    };

    if (m_config.allowTerminal)
    {
        // 终端是这台机器上唯一的执行面: ls/cat/mkdir/rm/cp/mv/grep 都是 shell 命令,
        // 由 terminal_run 覆盖; 这里只挂终端会话管理 + 一个通用 shell 工具。
        addTool(new JZTerminalRunTool(m_terminal));
        addTool(new JZTerminalQueryTool(m_terminal));
        addTool(new JZTerminalCancelTool(m_terminal));
        addTool(new JZTerminalOpenTool(m_terminal));
        addTool(new JZTerminalListTool(m_terminal));
        addTool(new JZTerminalCloseTool(m_terminal));
        addTool(new JZShellTool());
    }

    if (m_config.allowFileTools)
    {
        // 文件工具: 读带行号/分段、写整文件、局部精确替换(校验唯一匹配)、按名找文件、搜内容、时间。
        // 这些 shell 给不了(sed 不校验唯一匹配), 所以不走终端。
        addTool(new JZReadFileTool());
        addTool(new JZWriteFileTool());
        addTool(new JZModifyFileTool());
        addTool(new JZSearchFileTool());      // 按文件名/路径找文件(如 "*.d.ts")
        addTool(new JZSearchContextTool());   // 搜文本/正则(原 search_file, 已改名)
        addTool(new JZGetDateTimeTool());
    }

    if (m_config.allowNetworkTools)
    {
        // 联网工具: web_search(搜索) + web_fetch(抓正文)。两者共享同一个访问管理器,
        // 代理只在管理器上设一次(http / https 都生效)。
        // 没有宿主也能跑 —— 它们不出工作区, 也不需要 ICodeToolHost, 所以不参与区域审核。
        if (!m_netMan)
            m_netMan = new JZNetworkAccessManager(this);
        if (!m_config.proxyHost.empty())
        {
            JZProxyConfig pc;
            pc.host = m_config.proxyHost;
            pc.port = m_config.proxyPort;
            pc.user = m_config.proxyUser;
            pc.password = m_config.proxyPassword;
            m_netMan->setProxy(pc);
        }

        auto *searchTool = new JZWebSearchTool();
        if (!m_config.webSearchUrl.empty())
            searchTool->setSearchUrlTemplate(m_config.webSearchUrl);
        searchTool->setNetworkAccessManager(m_netMan);
        addTool(searchTool);

        auto *fetchTool = new JZWebFetchTool();
        fetchTool->setNetworkAccessManager(m_netMan);
        addTool(fetchTool);
    }

    if (m_config.memoryEnabled)
    {
        // 长期记忆: 跨任务的结论沉淀 —— 开工时注入任务消息(见 buildMemoryBlock),
        // 任务里由模型自己用 memory append 写。文件路径由宿主给定(模型改不了), 所以
        // 这个工具不参与路径审核; 配到工作区外就等于授权它往那儿写。
        m_memory = new JZMemoryTool();
        m_memory->setup(memoryFilePath(), m_config.memoryMaxBytes);
        addTool(m_memory);
    }
}

string JZAgentCli::buildSystemPrompt() const
{
    string text;
    text += "You are an autonomous coding agent running on the user's machine, driven "
            "via an HTTP task interface (submit a task, optionally with a verify script "
            "that gates completion).\n";
    text += "Working directory: " + m_config.rootDir + "  —— 相对路径以它为基准。\n";
    text += "Available tools:\n";
    if (m_config.allowTerminal)
    {
        text += "  terminal_run    执行一条 shell 命令(命令整条交给 shell: 管道/重定向/串联/通配符/变量都生效; 同一会话可同时跑多条)。\n";
        text += "  terminal_query  按 run_id 查一次运行的状态与日志(run_id 省略 = 最近一次运行; 后台起的、前台跑完的都能查)。\n";
        text += "  terminal_cancel 按 run_id 停掉一条还在跑的命令(run_id 省略 = 最近一条在跑的)。\n";
        text += "  terminal_open / terminal_list / terminal_close   多会话管理(单终端场景下可不用)。\n";
        text += "  command         在工作区内跑一条外部命令(带超时; 工具名是 command, 不是 shell)。\n";
    }
    if (m_config.allowFileTools)
    {
        text += "  read_file / write_file / modify_file   按行读(带行号)/整文件写/局部精确替换。\n";
        text += "  search_file    按文件名/路径找文件(通配符 * ?，如 \"*.d.ts\"、\"js_wrapper/*/types/*.d.ts\")，只回路径。\n";
        text += "  search_context 在工作区内搜索文本/正则(找某个串在哪些文件哪些行出现过)。\n";
        text += "  get_datetime   获取当前日期时间(需要\"今天\"这类信息时优先用)。\n";
    }
    if (m_config.allowNetworkTools)
    {
        text += "  web_search     联网搜索(给查询词, 回标题/链接/摘要): 查官方文档、API 用法、报错原因、"
                "第三方库这类需要实时信息的问题。\n";
        text += "  web_fetch      抓取某个 URL 的正文(转纯文本), 与 web_search 配合看某条结果的详情。\n";
    }
    if (m_config.memoryEnabled)
        text += "  memory         跨任务长期记忆: read / append / replace (" + memoryFilePath() + ")。\n";
    text += "\nShell notes (important):\n";
    if (m_terminal && !m_terminal->shellProgram().empty())
    {
        // 把探测到的 shell 告诉模型: 是 bash 才敢用 bash 专有语法; 回退成 sh 时得写 POSIX,
        // 否则模型会一直撞"语法错误"然后换做法(它自己意识不到解释器换了)。
        const string shell = m_terminal->shellProgram();
        const bool isBash = str_toLower(shell).find("bash") != string::npos;
        text += "  - 命令交给 " + shell + " 解释("
                + (isBash ? "bash: `[[ ]]`、数组、`local`、`source` 都能用"
                          : "POSIX sh: 用 `[ ]` 而不是 `[[ ]]`, 用 `.` 而不是 `source`")
                + ")。脚本自身看它的 shebang; 没有 shebang 时也按这个 shell 解释。\n";
    }
    text += "  - 每条命令都是一个新 shell: 要进目录执行写 `cd <dir> && <cmd>`; "
            "单独的 `cd` 会被拒绝。cd/export 不跨命令保留。\n";
    text += "  - 不支持后台 `&`、heredoc、进程替换、for/while/if 复合语句。\n";
    // "起服务再测"怎么跑必须说清楚: 模型容易给服务器那条一个很长的 timeout_ms 然后前台干等 ——
    // 那条要挂满超时才回(中途取不到输出), 而且超时那一刻服务就被杀掉了。正确做法是 background=true。
    text += jzTerminalRunNotes();
    text += "  - 优先用 read_file 而不是 cat, 用 modify_file 而不是 sed。\n";
    text += "  - 写权限: 工作区(读写执行)/授信目录(默认只读)/区外只读; 破坏性命令(dd/sudo...)被拒。\n";
    if (!m_config.programPaths.empty())
    {
        text += "\n本机程序(命令名 -> 全路径): 执行时会把命令里的命令名换成全路径, **只写命令名**(写全路径会被路径审核拒):\n";
        for (const auto &kv : m_config.programPaths)
            text += "  " + kv.first + "    " + kv.second + "\n";
    }
    // 配了 code_runtime = 本机就是 JZ 的 TS 运行时: 把它的模块/生命周期边界一并告诉模型。
    // 模型只会按 Node 的习惯写, 这些边界它猜不出来(猜错就是"编译过了但跑不起来")。
    if (m_config.programPaths.count("code_runtime"))
        text += "\n" + jzTsRuntimeNotes();
    if (m_config.memoryEnabled)
    {
        text += "\nMemory (跨任务的长期记忆):\n";
        text += "  - 每次任务的开头, 消息里可能带一段 <memory> —— 那是之前任务留下的结论, 先看再用。\n";
        text += "  - 得到值得跨任务复用的结论时, 用 memory 工具 append 记下来: 环境事实(构建/运行命令、"
                "程序路径、依赖版本)、踩过的坑、接口/目录约定、最终结论。一条一句, 写清\"是什么\"和"
                "\"为什么\"; 别记流水账(过程已经在对话历史里了)。\n";
        text += "  - 记忆可能过期: 与当前代码/文件冲突时以实际文件为准, 并把更正后的结论写回记忆"
                "(用 replace 精简, 别只往上堆)。\n";
    }
    // MCP 远端工具: 工具本身由 JZAgent 的 tools 参数带给模型, 这里只说清"名字带前缀的那些
    // 是什么、在哪执行"—— 配置里声明了但没连上时函数列表里就没有, 故措辞不写成"一定存在"。
    if (m_config.mcpEnabled && !m_config.mcpServers.empty())
    {
        text += "\nMCP remote tools:\n";
        text += "  本端还配置了若干个外部 MCP 服务器; 若函数列表里出现名字形如 <server>__<tool> 的工具,\n";
        text += "  那些就是它们提供的远端工具 —— 用法与本地工具完全一样, 但**在对方那台机器上执行**\n";
        text += "  (不经过本机的区域/路径审核), 出错时先看是不是那边的问题。配置的服务器:\n";
        for (const JZCliMcpServer &s : m_config.mcpServers)
            text += "    - " + (s.name.empty() ? s.transport : s.name) + "  (" + s.transport + ")\n";
    }
    // 宿主**常驻说明**(如"玩法任务书"): 只进 system prompt 一次 —— 之后每轮 /task 只叠加新消息,
    // 不用把同一段介绍重复贴一遍。extraFile(相对 rootDir)在前, 内联 extra 在后(可补充/覆盖)。
    string extra = m_config.systemExtra;
    if (!m_config.systemExtraFile.empty())
    {
        string p = m_config.systemExtraFile;
        if (!JZDir::isAbsolutePath(p))
            p = m_config.rootDir + "/" + p;
        p = codeNormalizePath(p);
        ifstream ifs(p, ios::binary);
        if (ifs)
        {
            stringstream ss;
            ss << ifs.rdbuf();
            extra = ss.str() + "\n" + extra;
        }
        else
            jzWarning() << "system.extraFile 读不到: " << p;
    }
    if (!extra.empty())
    {
        text += "\n===== 常驻说明（宿主给：本任务从头到尾都适用，优先级高于上面的通用说明）=====\n";
        text += extra;
        if (extra[extra.size() - 1] != '\n')
            text += "\n";
    }
    text += "\nWork through the user's request step by step using the tools. "
            "When done, give a concise summary of what you changed.\n";
    return text;
}

// ---------------------------------------------------------------------------
// 长期记忆
//   记忆是"跨任务"的: 进程内每个任务都会 reset 会话(提交前 m_agent->reset()),
//   所以结论必须落成文件才有意义 —— 落在工作区根目录, 换个人/换个 CLI 也能接着用。
// ---------------------------------------------------------------------------
string JZAgentCli::memoryFilePath() const
{
    string path = m_config.memoryFile.empty() ? string(".agent_memory.md") : m_config.memoryFile;
    if (!JZDir::isAbsolutePath(path))
        path = m_config.rootDir + "/" + path;
    return codeNormalizePath(path);
}

string JZAgentCli::buildMemoryBlock() const
{
    if (!m_memory || !m_memory->enabled())
        return string();

    int total = 0;
    bool cut = false;
    string mem = m_memory->readMemory(&total, &cut);
    if (str_trimmed(mem).empty())
        return string();

    // 注入时明确标出"这是记忆、可能过期": 记忆最危险的失败模式就是拿过期的旧结论
    // 覆盖当前代码 —— 所以每次都提醒它"以实际文件为准"。
    string s;
    s += "<memory file=\"" + m_memory->file() + "\" bytes=\"" + to_string(total) + "\"";
    if (cut)
        s += " truncated=\"true\"";
    s += ">\n";
    s += mem + "\n</memory>\n";
    s += "上面是之前任务留下的长期记忆(可能已经过期; 与当前代码/文件冲突时以实际文件为准)。"
         "本次如果得到值得跨任务复用的结论, 用 memory 工具 append 记下来。\n\n";
    return s;
}

// ==========================================================================
// HTTP 传输 + 验证脚本门控循环
// ==========================================================================

// 路由处理器: 把 /task 请求转交给 JZAgentCli::onTaskSubmit
class TaskHandler : public JZHttpHandler
{
public:
    explicit TaskHandler(JZAgentCli *cli) : m_cli(cli) {}
    void handle(JZHttpServerClient *client, const JZHttpRequest &req) override
    {
        m_cli->onTaskSubmit(client, req);
    }
private:
    JZAgentCli *m_cli;
};

// 路由处理器: 列出工作区内可当验证脚本的文件, 供网页下拉选择
class ScriptsHandler : public JZHttpHandler
{
public:
    explicit ScriptsHandler(JZAgentCli *cli) : m_cli(cli) {}
    void handle(JZHttpServerClient *client, const JZHttpRequest &req) override
    {
        m_cli->onListScripts(client, req);
    }
private:
    JZAgentCli *m_cli;
};

// 路由处理器: 实时事件流(GET /events, SSE)
class EventsHandler : public JZHttpHandler
{
public:
    explicit EventsHandler(JZAgentCli *cli) : m_cli(cli) {}
    void handle(JZHttpServerClient *client, const JZHttpRequest &req) override
    {
        m_cli->onEvents(client, req);
    }
private:
    JZAgentCli *m_cli;
};

// 路由处理器: 运行信息(GET /info)
class InfoHandler : public JZHttpHandler
{
public:
    explicit InfoHandler(JZAgentCli *cli) : m_cli(cli) {}
    void handle(JZHttpServerClient *client, const JZHttpRequest &req) override
    {
        m_cli->onInfo(client, req);
    }
private:
    JZAgentCli *m_cli;
};

// 路由处理器: 后台任务面板(GET /terminal, GET /terminal/log, POST /terminal/cancel)
class TerminalListHandler : public JZHttpHandler
{
public:
    explicit TerminalListHandler(JZAgentCli *cli) : m_cli(cli) {}
    void handle(JZHttpServerClient *client, const JZHttpRequest &req) override
    {
        m_cli->onTerminalList(client, req);
    }
private:
    JZAgentCli *m_cli;
};

class TerminalLogHandler : public JZHttpHandler
{
public:
    explicit TerminalLogHandler(JZAgentCli *cli) : m_cli(cli) {}
    void handle(JZHttpServerClient *client, const JZHttpRequest &req) override
    {
        m_cli->onTerminalLog(client, req);
    }
private:
    JZAgentCli *m_cli;
};

class TerminalCancelHandler : public JZHttpHandler
{
public:
    explicit TerminalCancelHandler(JZAgentCli *cli) : m_cli(cli) {}
    void handle(JZHttpServerClient *client, const JZHttpRequest &req) override
    {
        m_cli->onTerminalCancel(client, req);
    }
private:
    JZAgentCli *m_cli;
};

// 路由处理器: 重置会话(POST /reset)
class ResetHandler : public JZHttpHandler
{
public:
    explicit ResetHandler(JZAgentCli *cli) : m_cli(cli) {}
    void handle(JZHttpServerClient *client, const JZHttpRequest &req) override
    {
        m_cli->onReset(client, req);
    }
private:
    JZAgentCli *m_cli;
};

void JZAgentCli::startHttp()
{
    m_http = new JZHttpServer(this);

    // 网页控制台(聊天式 UI): 从 httpDir 提供 index.html / app.js / style.css
    std::string httpDir = m_config.httpDir;
    if (httpDir.empty())
        httpDir = JZPlatform::applicationDirPath() + "/http";
    auto *staticFiles = new JZHttpStaticFiles(httpDir);
    m_http->addRoute("/", staticFiles);
    m_http->addRoute("/<path:path>", staticFiles);

    // 供网页下拉选择验证脚本
    m_http->addRoute("/scripts", new ScriptsHandler(this));

    // 网页顶栏显示: 模型地址 / 模型名 / 工作区
    m_http->addRoute("/info", new InfoHandler(this));

    m_http->addRoute("/task", new TaskHandler(this));
    m_http->addRoute("/reset", new ResetHandler(this));
    m_http->addRoute("/events", new EventsHandler(this));

    // 后台任务面板(网页): 列运行 / 取日志 / 停一条 —— 人想"看看服务在打什么"时不用去问模型
    m_http->addRoute("/terminal", new TerminalListHandler(this));
    m_http->addRoute("/terminal/log", new TerminalLogHandler(this));
    m_http->addRoute("/terminal/cancel", new TerminalCancelHandler(this));
    if (!m_http->listen(m_config.httpBind, m_config.httpPort))
        jzWarning() << "HttpServer 监听失败:" << m_config.httpBind << ":" << m_config.httpPort;
}

// ==========================================================================
// FRP 内网穿透(client 角色 = frpc)
//   本端连到配置里的远端 frps, 把本地服务(如本机 HTTP/admin 接口)通过代理暴露到
//   远端 frps 的 remotePort 上 —— 这样在外网/其它机器上访问 <frps>:<remotePort>
//   就能转发到本机的 <localIp>:<localPort>。
//   ⚠ 服务端(frps)由 JZAgentCliServer(见 server/)承担; 这里只做客户端。
//   frpc 靠 JZNetPack 收发包, 显式写上包类型注册(与 binary/frpc 的 jzfrpc 一致)。
//   包类型其实在 JZApplication 构造(JZDriver::init -> JZNetPackInit, 内含 JZFrpPacketInit)
//   时就注册好了 ⇒ 这里是第二次调 —— 两个入口都有 once 保护, 重复调用无害且不重复注册。
// ==========================================================================
void JZAgentCli::startFrpc()
{
    // frpc 客户端: 本端连到远端 frps, 把本地服务经代理暴露出去。
    if (!m_config.frp.client.enabled)
        return;

    if (m_config.frp.client.serverAddr.empty())
    {
        jzWarning() << "frp.client 已启用但 serverAddr 为空, 跳过 frpc 启动";
        return;
    }
    if (m_config.frp.client.proxies.empty())
    {
        jzWarning() << "frp.client 已启用但没有任何 proxy, 跳过 frpc 启动";
        return;
    }

    JZFrpClientConfig cc;
    cc.serverIp = m_config.frp.client.serverAddr;
    cc.serverPort = m_config.frp.client.serverPort;
    cc.token = m_config.frp.client.token;
    for (const auto &p : m_config.frp.client.proxies)
    {
        if (p.localPort > 0 && p.remotePort > 0)
            cc.addProxy(p.name.empty() ? ("proxy" + to_string(cc.proxyList.size()))
                                        : p.name,
                        p.localIp.empty() ? std::string("127.0.0.1") : p.localIp,
                        p.localPort, p.remotePort);
    }
    if (cc.proxyList.empty())
    {
        jzWarning() << "frp.client 所有 proxy 的 localPort/remotePort 无效, 跳过 frpc 启动";
        return;
    }

    m_frpClient = new JZFrpClient(this);
    m_frpClient->setConfig(cc);
    m_frpClient->connectToServer();   // 异步连接 + 内置 15s 重连, 不阻塞主线程
    jzInfo() << "FrpClient(frpc) 已连接远端 frps:" << cc.serverIp << ":" << cc.serverPort
             << "  代理数:" << cc.proxyList.size();
}

// ==========================================================================
// MCP 客户端(把外部 MCP 服务器的工具挂进本 agent)
//   配置驱动: 加减 server 只改 config.json(不用重编译); 启动时逐个连, 连上才登记工具。
//   两条必须知道的约束(详见 JZAgentCli.h 的 JZCliMcpServer):
//     ① 挂载只能在 Inactive(首个任务之前): JZAgent 拒绝在会话开始后改工具表
//        (会破坏 KV 前缀缓存, 见 rejectWhileActive) —— 晚到的连接只记警告;
//     ② 远端工具在对端执行, 不受本机 terminal 的区域/路径审核约束。
// ==========================================================================
void JZAgentCli::startMcp()
{
    if (!m_config.mcpEnabled || m_config.mcpServers.empty())
    {
        m_mcpInitDone = true;   // 没配 MCP: 不挡任务
        return;
    }

    m_mcpStates.clear();
    m_mcpStates.reserve(m_config.mcpServers.size());

    // 闸门: 从这一刻起新的 POST /task 先等着 —— 等下面每个连接都有结果(连上 / 连不上)。
    // 为什么必须等: JZAgent 只允许在 Inactive(首个任务之前)改工具表, 任务先跑起来以后
    // 连上的远端工具就挂不进去了(只能重启进程), 那还不如让提交方稍等几秒。
    m_mcpInitDone = false;
    m_mcpPending = 0;

    for (size_t i = 0; i < m_config.mcpServers.size(); i++)
    {
        const JZCliMcpServer &cfg = m_config.mcpServers[i];
        const std::string transport = str_toLower(cfg.transport);

        // 每条配置都先记一条状态(哪怕连不起来), 好让 /info 与日志能对账
        JZCliMcpState st;
        st.name = cfg.name.empty() ? transport : cfg.name;
        st.transport = transport;
        if (transport == "http")
            st.target = cfg.url;
        else
        {
            st.target = cfg.command;
            for (size_t k = 0; k < cfg.args.size(); k++)
                st.target += " " + cfg.args[k];
        }
        m_mcpStates.push_back(st);
        JZCliMcpState &state = m_mcpStates.back();

        // 配置校验在 loadConfig 已做过一遍, 这里再挡一次(防别的入口塞进来的配置)
        if (transport != "stdio" && transport != "http")
        {
            state.error = "transport 不支持(要用 stdio 或 http): " + cfg.transport;
            jzWarning() << "MCP server" << state.name << state.error;
            continue;
        }
        if (transport == "stdio" && cfg.command.empty())
        {
            state.error = "stdio 缺 command";
            jzWarning() << "MCP server" << state.name << state.error;
            continue;
        }
        if (transport == "http" && cfg.url.empty())
        {
            state.error = "http 缺 url";
            jzWarning() << "MCP server" << state.name << state.error;
            continue;
        }

        // 不给 parent: addMcp 要求接管时 client 还没有 parent(见 JZAgent::addMcp)。
        auto *client = new JZMcpClient();
        if (!cfg.toolPrefix.empty())
            client->setToolPrefix(cfg.toolPrefix);
        if (cfg.callTimeoutMs > 0)
            client->setCallTimeout(cfg.callTimeoutMs);

        // 先接好信号再发起连接(连接是异步的, 结束统一走 sigFinished)
        const int index = (int)i;
        connect(&client->sigFinished, [this, client, index]() { onMcpFinished(client, index); });

        const bool started = (transport == "http")
            ? client->startHttp(cfg.url, cfg.timeoutMs)
            : client->startProcess(cfg.command, cfg.args, cfg.timeoutMs);
        if (!started)
        {
            state.error = "发起连接失败: " + client->errorString();
            jzWarning() << "MCP" << state.name << state.error;
            client->deleteLater();
            continue;
        }
        m_mcpPending++;
        jzInfo() << "MCP 连接中:" << state.name
                 << (transport == "http" ? cfg.url : state.target);
    }

    // 一条都没发起成功(纯配置错误): 没有在途连接, 立刻放行, 别把任务卡死
    if (m_mcpPending == 0)
        m_mcpInitDone = true;
    else
        jzInfo() << "MCP 初始化中:" << m_mcpPending << " 个连接 —— 完成前提交任务会被拒绝";
}

void JZAgentCli::onMcpFinished(JZMcpClient *client, int index)
{
    // ⚠ 本函数**每个出口**都要走 finishMcpPending() 记账 —— 漏一个, 闸门就永远开不了。
    JZCliMcpState *state = (index >= 0 && index < (int)m_mcpStates.size())
                               ? &m_mcpStates[index] : nullptr;
    const std::string name = (state && !state->name.empty()) ? state->name : std::string("?");

    if (!client->isReady())
    {
        if (state)
            state->error = "连接失败: " + client->errorString();
        jzWarning() << "MCP 连接失败:" << name << client->errorString();
        client->deleteLater();
        finishMcpPending();
        return;
    }

    // addMcp 在 agent 非 Inactive 时会直接拒绝(只打 warning), 用 parent 判断是否真的接管了。
    m_agent->addMcp(client);
    if (client->parent() != m_agent)
    {
        if (state)
            state->error = "挂载被拒: agent 已经有会话在跑(改工具表会作废 KV 前缀缓存), 需重启进程";
        jzWarning() << "MCP 工具挂载被拒:" << name
                    << " —— agent 已有会话在跑, 这个 server 的工具要重启进程才会生效";
        client->deleteLater();
        finishMcpPending();
        return;
    }

    std::string tools;
    for (const JZAgentTool *tool : client->tools())
    {
        if (!tools.empty())
            tools += ", ";
        tools += tool->toolName();
    }
    if (state)
    {
        state->ready = true;
        state->tools = (int)client->tools().size();
    }
    jzInfo() << "MCP 已挂载:" << client->serverName() << client->serverVersion()
             << "  工具数:" << (int)client->tools().size()
             << (tools.empty() ? std::string() : ("  [" + tools + "]"));
    finishMcpPending();
}

// 一个 MCP 连接流程结束了: 未结束数 -1; 减到 0 = 配置里的 server 全部有结果(连上或连不上),
// 此时放行新任务(见 submitTask 的 m_mcpInitDone 检查)。
void JZAgentCli::finishMcpPending()
{
    if (m_mcpPending > 0)
        m_mcpPending--;
    if (m_mcpPending == 0 && !m_mcpInitDone)
    {
        m_mcpInitDone = true;
        jzInfo() << "MCP 初始化完毕 —— 现在可以提交任务了";
    }
}

Json::Value JZAgentCli::makeInfo() const
{
    Json::Value j;
    j["agent"] = m_config.agentName;
    j["version"] = m_config.version;
    j["modelUrl"] = m_config.modelUrl;
    j["modelName"] = m_config.modelName;
    j["rootDir"] = m_config.rootDir;
    j["mcpInitDone"] = m_mcpInitDone;   // false = 还在连 MCP, 此时的 POST /task 会被拒绝
    // 宿主常驻说明(system.extra / extraFile): 拼进 system prompt 一次, 之后每轮 /task 只叠加新消息 ——
    // 这里给出"从哪来 / 多大 / system prompt 总共多大", 便于自检(HTTP 模式默认不打 stdout)
    j["systemExtraFile"] = m_config.systemExtraFile;
    j["systemExtraBytes"] = (int)m_config.systemExtra.size();
    j["systemPromptBytes"] = m_systemPromptBytes;
    // MCP: 配置里每个 server 的连接/挂载状态(没有网页面板时用 GET /info 自检)
    if (!m_mcpStates.empty())
    {
        Json::Value mcp(Json::arrayValue);
        for (const JZCliMcpState &s : m_mcpStates)
        {
            Json::Value item(Json::objectValue);
            item["name"] = s.name;
            item["transport"] = s.transport;
            item["target"] = s.target;
            item["ready"] = s.ready;
            item["tools"] = s.tools;
            item["error"] = s.error;
            mcp.append(item);
        }
        j["mcp"] = mcp;
    }
    return j;   // 注意: 不含 modelToken, 别把凭据吐给网页
}

void JZAgentCli::onInfo(JZHttpServerClient *client, const JZHttpRequest &req)
{
    if (!checkToken(req))
    {
        sendJson(client, 401, Json::objectValue);
        return;
    }

    Json::Value j = makeInfo();
    j["busy"] = (bool)m_busy;
    j["round"] = m_loopRound;
    j["recent"] = recentJson();      // 最近若干次请求与结果(网页刚打开时靠它补回历史)
    sendJson(client, 200, j);
}

void JZAgentCli::onTerminalList(JZHttpServerClient *client, const JZHttpRequest &req)
{
    if (!checkToken(req))
    {
        sendJson(client, 401, Json::objectValue);
        return;
    }

    Json::Value arr(Json::arrayValue);
    int running = 0;
    if (m_terminal)
    {
        running = (int)m_terminal->activeRunIds().size();
        int64_t now = JZPlatform::getTickCountMs();
        std::vector<std::string> ids = m_terminal->runIds();   // 按发生先后
        // 新的在前, 最多 30 条(网页那一栏放不下更多; 逐条日志要单独取)
        for (size_t i = ids.size(); i-- > 0 && arr.size() < 30;)
        {
            JZVirtualTerminal::RunRecordInfo info;
            if (!m_terminal->runSnapshot(ids[i], info))
                continue;
            int64_t end = info.endTime > 0 ? info.endTime : now;
            Json::Value o;
            o["run_id"] = info.runId;
            o["command"] = info.command;
            o["status"] = info.status;
            o["background"] = info.background;
            o["pid"] = info.pid;
            o["exit_code"] = info.exitCode;
            o["timed_out"] = info.timedOut;
            o["error"] = info.error;
            o["elapsed_ms"] = (Json::Int64)(end - info.startTime);
            o["lines"] = (Json::UInt)info.totalLines;
            arr.append(o);
        }
    }

    Json::Value r;
    r["running"] = running;
    r["runs"] = arr;
    sendJson(client, 200, r);
}

void JZAgentCli::onTerminalLog(JZHttpServerClient *client, const JZHttpRequest &req)
{
    if (!checkToken(req))
    {
        sendJson(client, 401, Json::objectValue);
        return;
    }

    std::string runId = req.queryParam("run_id");
    JZVirtualTerminal::RunRecordInfo info;
    if (!m_terminal || runId.empty() || !m_terminal->runSnapshot(runId, info))
    {
        Json::Value e;
        e["error"] = "没有这个 run_id: " + runId;
        sendJson(client, 404, e);
        return;
    }

    int tail = atoi(req.queryParam("tail").c_str());
    if (tail <= 0)
        tail = 200;
    std::string grep = req.queryParam("grep");
    std::string stream = str_toLower(req.queryParam("stream"));
    if (stream == "all")
        stream.clear();

    // 与模型侧的 terminal_query 同一套过滤, 但**不碰增量游标**(那是模型的读位置, 网页别去动它)
    const size_t kMaxBytes = 64 * 1024;
    std::string text;
    size_t shown = 0;
    size_t from = 0;
    if (info.log.size() > (size_t)tail)
        from = info.log.size() - (size_t)tail;
    for (size_t i = from; i < info.log.size(); i++)
    {
        const JZVirtualTerminal::RunLogLine &l = info.log[i];
        if (!stream.empty() && l.stream != stream)
            continue;
        if (!grep.empty() && l.text.find(grep) == std::string::npos)
            continue;
        std::string one = l.stream + "| " + l.text + "\n";
        if (text.size() + one.size() > kMaxBytes)
        {
            text += "...(日志过大, 只显示到这里; 用 grep 缩小范围)\n";
            break;
        }
        text += one;
        shown++;
    }

    Json::Value r;
    r["run_id"] = info.runId;
    r["command"] = info.command;
    r["status"] = info.status;
    r["exit_code"] = info.exitCode;
    r["timed_out"] = info.timedOut;
    r["background"] = info.background;
    r["pid"] = info.pid;
    r["total_lines"] = (Json::UInt)info.totalLines;
    r["shown"] = (Json::UInt)shown;
    r["dropped"] = (Json::UInt)info.firstLine;    // 记录上限丢掉的更早行数
    r["truncated"] = (from > 0);
    r["text"] = text;
    sendJson(client, 200, r);
}

void JZAgentCli::onTerminalCancel(JZHttpServerClient *client, const JZHttpRequest &req)
{
    if (!checkToken(req))
    {
        sendJson(client, 401, Json::objectValue);
        return;
    }

    std::string runId = req.queryParam("run_id");
    if (runId.empty())
    {
        // 也允许 POST body: {"run_id":"xxx"}
        Json::Value root;
        Json::Reader reader;
        if (reader.parse(std::string(req.body().data(), req.body().size()), root) && root.isObject())
            runId = root.get("run_id", "").asString();
    }

    bool ok = false;
    int killed = 0;
    if (m_terminal)
    {
        if (!runId.empty())
            ok = m_terminal->cancel(runId);
        else
        {
            killed = m_terminal->cancelAll();   // 没指定 = 停所有在跑的(网页"全停")
            ok = (killed > 0);
        }
    }

    Json::Value r;
    r["ok"] = ok;
    r["run_id"] = runId;
    r["killed"] = killed;
    if (ok)
    {
        Json::Value e;
        e["run_id"] = runId;
        e["killed"] = killed;
        pushEvent("terminal_cancel", e);   // 其它开着的页面也刷新一下
    }
    sendJson(client, ok ? 200 : 409, r);
}

void JZAgentCli::sendJson(JZHttpServerClient *client, int code, const Json::Value &j)
{
    JZHttpRespone resp;
    resp.setStatusCode(code);
    resp.setJson(Json::FastWriter().write(j));
    client->response(resp.toByteArray());
}

// ==========================================================================
// 实时事件流(GET /events, SSE) —— 任务进行中的即时反馈
// ==========================================================================

// SSE 帧 -> HTTP chunked 分块(长度前缀 + 帧 + CRLF)。
// /events 声明了 Transfer-Encoding: chunked, 每帧必须按分块格式写, 否则客户端解不出帧边界。
static std::string makeSseChunk(const std::string &payload)
{
    std::string frame = "data: " + payload + "\n\n";
    char head[32];
    snprintf(head, sizeof(head), "%x\r\n", (unsigned)frame.size());
    std::string chunk = head;
    chunk += frame;
    chunk += "\r\n";
    return chunk;
}

void JZAgentCli::onEvents(JZHttpServerClient *client, const JZHttpRequest &req)
{
    if (!checkToken(req))
    {
        sendJson(client, 401, Json::objectValue);
        return;
    }

    // 响应头不带 Content-Length; 用 chunked 分块 —— 浏览器 EventSource 两种都能实时收,
    // 但只有 chunked 才能让**非浏览器**客户端(如 JZNetworkReply)逐帧读到: 无 Content-Length
    // 的流式响应在那些客户端里要等连接关闭才一次性交付, 过程就全憋到最后了。
    JZHttpRespone resp;
    resp.setStatusCode(200);
    resp.setContentType("text/event-stream");
    resp.setHeader("Cache-Control", "no-cache");
    resp.setHeader("Connection", "keep-alive");
    resp.setHeader("X-Accel-Buffering", "no");
    resp.setHeader("Transfer-Encoding", "chunked");
    client->write(resp.toHeadByteArray(0, false), false);

    m_eventClients.push_back(client);
    connect(&client->sigDestroyed, this, [this, client](JZObject *) {
        removeEventClient(client);
    });

    Json::Value d = makeInfo();
    d["busy"] = (bool)m_busy;
    d["round"] = m_loopRound;
    // 连接上来时已经闲着 + 留着上次结果: 带上, 让"断开期间跑完"的任务能在这边收尾
    // (典型: 网页刷新时 /task 的连接被 abort, 任务其实继续跑完了 —— 见 onClientDestroyed)。
    if (!m_busy && m_hasLastResult)
        d["last_result"] = m_lastResult;
    d["recent"] = recentJson();      // 最近若干次请求与结果: 新开的网页靠它把历史卡片补回来
    Json::Value first;
    first["type"] = "hello";
    first["data"] = d;
    std::string payload = Json::FastWriter().write(first);
    while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r'))
        payload.pop_back();
    std::string chunk = makeSseChunk(payload);
    client->write(chunk.data(), (int)chunk.size(), false);

    // 正有任务在跑: 推一份当前状态快照, 让刚打开/刷新网页的人也能看到进行中的任务
    // (prompt / 已累积的思考·回复 / 在途工具调用)。随后该任务的实时事件会接着这个快照续推。
    if (m_busy)
    {
        // 思考/回复可能跨多轮累积得很大, 快照只取尾部, 避免单帧过大(界面展示也只取尾部)
        const size_t kSnapCap = 6000;
        auto tail = [&](const std::string &s) -> std::string {
            return s.size() <= kSnapCap ? s : ("…(前面已省略)\n" + s.substr(s.size() - kSnapCap));
        };

        Json::Value s;
        s["busy"] = true;
        s["round"] = m_loopRound;
        s["verify_cmd"] = m_verifyCmd;
        s["prompt"] = m_currentPrompt;
        s["reply"] = tail(m_accumReply);
        s["think"] = tail(m_accumThink);
        s["last_verify_output"] = m_lastVerifyOutput;
        Json::Value tools(Json::arrayValue);
        for (const auto &t : m_toolSnaps)
        {
            Json::Value o;
            o["id"] = t.id;
            o["name"] = t.name;
            o["args"] = t.args;
            if (t.hasResult)
            {
                o["ok"] = t.ok;
                o["result"] = t.result;
            }
            tools.append(o);
        }
        s["tools"] = tools;
        pushEvent("state", s);   // 广播给所有订阅者(含本连接)
    }
}

void JZAgentCli::removeEventClient(JZHttpServerClient *client)
{
    for (auto it = m_eventClients.begin(); it != m_eventClients.end(); )
    {
        if (*it == client)
            it = m_eventClients.erase(it);
        else
            ++it;
    }
}

void JZAgentCli::pushEvent(const std::string &type, const Json::Value &data)
{
    if (m_eventClients.empty())
        return;

    Json::Value j;
    j["type"] = type;
    j["data"] = data;
    std::string payload = Json::FastWriter().write(j);
    while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r'))
        payload.pop_back();
    std::string chunk = makeSseChunk(payload);
    for (JZHttpServerClient *c : m_eventClients)
        c->write(chunk.data(), (int)chunk.size(), false);
}

bool JZAgentCli::checkToken(const JZHttpRequest &req) const
{
    if (m_config.httpToken.empty())
        return true;   // 未配置 token: 不鉴权

    std::string tk = req.queryParam("token");
    if (tk.empty())
    {
        std::string auth = req.header("Authorization");
        if (auth.size() > 7 && str_toLower(auth.substr(0, 7)) == "bearer ")
            tk = auth.substr(7);
    }
    return tk == m_config.httpToken;
}

void JZAgentCli::onTaskSubmit(JZHttpServerClient *client, const JZHttpRequest &req)
{
    // 1) token 校验(若配置了 httpToken)
    if (!checkToken(req))
    {
        sendJson(client, 401, Json::objectValue);
        return;
    }

    // 2) 解析 JSON body
    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(std::string(req.body().data(), req.body().size()), root) || !root.isObject())
    {
        Json::Value e; e["error"] = "invalid JSON body";
        sendJson(client, 400, e);
        return;
    }

    // 3) 基本校验(空任务 / 图片不合法 / 忙) -> 立即回包
    string preImgError;
    std::vector<std::string> preImages = extractImages(root, preImgError);
    if (!preImgError.empty())
    {
        Json::Value e; e["error"] = preImgError;
        sendJson(client, 400, e);
        return;
    }
    if (extractUserText(root).empty() && preImages.empty())
    {
        Json::Value e; e["error"] = "no prompt found (need 'prompt' / 'messages', or 'images')";
        sendJson(client, 400, e);
        return;
    }
    if (m_busy)
    {
        Json::Value e; e["error"] = "a task is already running";
        sendJson(client, 409, e);
        return;
    }

    // 4) 进入任务循环, 结果经 HTTP 回包(200)
    m_pendingClient = client;
    connect(static_cast<JZSignal<JZObject*>*>(&client->sigDestroyed), this, &JZAgentCli::onClientDestroyed);
    submitTask(root, [this, client](const Json::Value &j) {
        sendJson(client, 200, j);
    });
}

// 任务提交核心: 由 HTTP(onTaskSubmit) 调用, 结果经 respond 回调回包。
void JZAgentCli::submitTask(const Json::Value &req, std::function<void(const Json::Value &)> respond)
{
    if (m_busy)
    {
        Json::Value e; e["error"] = "a task is already running";
        if (respond) respond(e);
        return;
    }
    // MCP 还没初始化完(有 server 没连出结果): 先拒绝 —— 任务一旦开跑, 稍后连上的远端工具
    // 就挂不进 agent 了(JZAgent 只允许在 Inactive 改工具表), 那还不如让提交方等几秒再来。
    if (!m_mcpInitDone)
    {
        std::string waiting;
        for (const JZCliMcpState &s : m_mcpStates)
        {
            if (s.ready || !s.error.empty())
                continue;   // 已就绪 / 已有结论(失败) 的都不算在等
            if (!waiting.empty())
                waiting += ", ";
            waiting += s.name;
        }
        Json::Value e;
        e["error"] = "MCP 初始化中("
                     + (waiting.empty() ? (to_string(m_mcpPending) + " 个连接") : waiting)
                     + "), 稍后再提交 —— 连不上也没关系, 全部有结果后就会放行";
        e["mcp_pending"] = m_mcpPending;
        if (respond) respond(e);
        return;
    }
    std::string imgError;
    std::vector<std::string> images = extractImages(req, imgError);
    if (!imgError.empty())
    {
        Json::Value e; e["error"] = imgError;
        if (respond) respond(e);
        return;
    }
    std::string prompt = extractUserText(req);
    // 文字与图片都空才算空任务(允许"只发截图", 与 code_agent 的 request() 同一口径)
    if (prompt.empty() && images.empty())
    {
        Json::Value e; e["error"] = "no prompt found (need 'prompt' / 'messages', or 'images')";
        if (respond) respond(e);
        return;
    }

    // 长期记忆: 开工时把记忆注入任务消息(不是 system prompt —— 那个是进程级的, 改一次
    // 就把 KV 前缀缓存整段作废)。use_memory=false 可单次关掉(评测/一次性任务用)。
    std::string memoryBlock;
    if (req.isMember("use_memory") ? req["use_memory"].asBool() : true)
        memoryBlock = buildMemoryBlock();

    std::string script = req.isMember("verify_script") ? req["verify_script"].asString() : "";
    std::string explicitCmd = req.isMember("verify_cmd") ? req["verify_cmd"].asString() : "";
    m_verifyCmd = deriveVerifyCmd(script, explicitCmd);
    // 默认值来自配置(verify.maxRounds / verify.timeoutMs), 本次请求可覆盖
    m_verifyTimeoutMs = req.isMember("verify_timeout") ? req["verify_timeout"].asInt() : m_config.verifyTimeoutMs;
    m_maxLoopRounds = req.isMember("max_verify_rounds") ? req["max_verify_rounds"].asInt() : m_config.verifyMaxRounds;
    if (m_verifyTimeoutMs <= 0) m_verifyTimeoutMs = 60000;
    if (m_maxLoopRounds <= 0) m_maxLoopRounds = 30;

    // 出错自动重试: 默认取配置(llm.maxRetries / llm.maxFixRetries / llm.retryDelayMs), 本次请求可覆盖
    m_maxErrorRetries = req.isMember("max_retries") ? req["max_retries"].asInt() : m_config.maxRetries;
    m_maxFixRetries = req.isMember("max_fix_retries") ? req["max_fix_retries"].asInt() : m_config.maxFixRetries;
    m_retryDelayMs = req.isMember("retry_delay_ms") ? req["retry_delay_ms"].asInt() : m_config.retryDelayMs;
    if (m_maxErrorRetries < 0) m_maxErrorRetries = 0;
    if (m_maxFixRetries < 0) m_maxFixRetries = 0;
    if (m_retryDelayMs < 0) m_retryDelayMs = 0;

    m_respond = std::move(respond);
    m_loopActive = true;
    m_loopRound = 0;
    m_accumReply.clear();
    m_thinkLogged = false;
    m_lastVerifyOutput.clear();
    m_currentPrompt = prompt;          // 记录 prompt, 供连接快照
    m_accumThink.clear();
    m_thinkLogged = false;
    m_toolSnaps.clear();
    m_busy = true;
    m_failed = false;
    m_cancelled = false;
    m_lastError.clear();
    m_lastErrorCode = JZAgent::None;
    m_errorRetries = 0;
    m_fixRetries = 0;
    m_retryPending = false;
    m_lastFeedbackSig.clear();
    m_sameFailCount = 0;
    m_loopEpoch++;          // 新任务: 作废上一个任务残留的延时重试

    Json::Value start;
    start["verify_cmd"] = m_verifyCmd;
    start["max_rounds"] = m_maxLoopRounds;
    start["max_retries"] = m_maxErrorRetries;
    start["max_fix_retries"] = m_maxFixRetries;
    start["memory_bytes"] = (int)memoryBlock.size();      // 0 = 没带记忆(未启用/还没记忆/本次关掉)
    start["images"] = (int)images.size();                 // 附图张数(网页/脚本据此提示; 图本身不进事件流)
    pushEvent("task_start", start);
    pushHistory(prompt, m_verifyCmd);   // 记一条"最近任务"(running), 收尾时回填结果

    // 多模态: 图片与文字同属这一条 user 消息(走 JZAgent 的 request(text, images) 重载, 与桌面版同一条路)。
    // 后续续跑/验证失败的喂回仍是纯文本 —— 图只在首轮给一次。
    agentLog("input: " + logTrunc(memoryBlock + prompt));
    if (images.empty())
        m_agent->request(memoryBlock + prompt);
    else
        m_agent->request(memoryBlock + prompt, images);
    m_sessionStarted = true;
    // 注意: 不在此回包; onLoopFinished 在 agent 结束后被调用并回包(经 m_respond)。
}

// ---------------------------------------------------------------------------
// "多轮"由**外部驱动**, 不在这里再搞一个 --loop 模式:
//   一轮 = 一次 POST /task(prompt + verify_cmd), 走的就是上面 submitTask 那一套(同一会话、
//   同一"验证脚本门控"循环)。跑几轮 / 什么时候停, 是**提交方**的事 —— 验证程序在自己输出里
//   打 [loop-stop], 提交方看见后不再提交下一轮即可; CLI 不需要认识任何标记, 也不需要
//   第二个循环模式(它俩本来就是同一条链路的两种驱动方式, 分成两套只会各修一遍)。
//   落地见 binary/agent_cli/tools/agent_loop.py: 拉起 agent_cli(HTTP) + 逐轮提交 + 外部复验。
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// 验证命令的解释器选择//   Windows(cmd)下 `python3` / `bash` 经常根本不存在 —— 装了 Python 往往只有 python.exe,
//   Git Bash 也没进 PATH。硬写 python3 会让验证脚本每一轮都以 9009/127 退出(看着像"测试
//   一直失败"), 白烧掉所有续跑轮次。所以推导时先挑一个"本机真能跑"的, 顺序:
//     ① 配置的程序映射(terminal.programs, 如 {"python":"C:/.../python.exe"})
//     ② 候选名逐个探测(`<name> --version` 退出码 0 算能跑)
//     ③ 都没有就退回默认名(保持旧行为: 至少错误信息会在验证输出里露出来)
//   探测结果进程内缓存(每条任务 spawn 一次探测进程没必要)。
// ---------------------------------------------------------------------------
static bool programRunnable(const std::string &name)
{
    static std::map<std::string, bool> cache;
    std::map<std::string, bool>::iterator it = cache.find(name);
    if (it != cache.end())
        return it->second;

#ifdef _WIN32
    std::string probe = "\"" + name + "\" --version > nul 2>&1";
    FILE *f = _popen(probe.c_str(), "r");
#else
    std::string probe = "\"" + name + "\" --version > /dev/null 2>&1";
    FILE *f = popen(probe.c_str(), "r");
#endif
    bool ok = false;
    if (f)
    {
        char buf[256];
        while (std::fgets(buf, sizeof(buf), f)) { }
#ifdef _WIN32
        ok = (_pclose(f) == 0);
#else
        ok = (pclose(f) == 0);
#endif
    }
    cache[name] = ok;
    return ok;
}

// 含空格的全路径要引号(cmd 会把 "C:/Program Files/..." 截在空格处)
static std::string quotePath(const std::string &p)
{
    if (p.find(' ') != std::string::npos && p.find('/') != std::string::npos)
        return "\"" + p + "\"";
    return p;
}

std::string JZAgentCli::resolveProgram(const std::vector<std::string> &cands, const std::string &fallback) const
{
    for (size_t i = 0; i < cands.size(); ++i)
    {
        const std::string &c = cands[i];
        // 全路径(含空格)整体当程序名; 否则按第一个空格拆掉参数("py -3" -> "py")
        std::string name = (c.find('/') != std::string::npos) ? c : c.substr(0, c.find(' '));

        std::map<std::string, std::string>::const_iterator it = m_config.programPaths.find(name);
        if (it != m_config.programPaths.end() && JZFileInfo::exists(it->second))
            return quotePath(it->second) + c.substr(name.size());   // 配置映射优先

        if (programRunnable(name))
            return (name == c) ? quotePath(c) : c;   // 带参数的候选原样返回
    }
    return fallback;
}

std::string JZAgentCli::deriveVerifyCmd(const std::string &script, const std::string &explicitCmd) const
{
    if (!explicitCmd.empty())
        return explicitCmd;      // 显式命令: 调用方自己给的, 原样用(包括它选定的解释器)
    if (script.empty())
        return "";
    size_t dot = script.find_last_of('.');
    std::string ext = (dot != std::string::npos) ? str_toLower(script.substr(dot + 1)) : "";
    std::string q = "\"" + script + "\"";   // 路径含空格时保险
    if (ext == "exe") return q;

    if (ext == "py")
        return resolveProgram({"python3", "python", "py -3"}, "python3") + " " + q;

    // sh 及未知后缀按 shell 脚本处理; 终端配了 shell 就优先用它
    std::vector<std::string> shells;
    if (!m_config.shellProgram.empty())
        shells.push_back(m_config.shellProgram);
    shells.push_back("bash");
#ifdef _WIN32
    shells.push_back("C:/Program Files/Git/bin/bash.exe");
    shells.push_back("C:/Program Files/Git/usr/bin/bash.exe");
    shells.push_back("C:/Program Files (x86)/Git/bin/bash.exe");
#endif
    return resolveProgram(shells, "bash") + " " + q;
}

void JZAgentCli::runVerifyAsync(const std::string &cmd, int timeoutMs, const std::string &cwd,
                                const std::function<void(int, const std::string &)> &cb)
{
    m_verifyCb = cb;
    m_verifyOut.clear();
    m_verifyTimedOut = false;
    const int gen = ++m_verifyGen;
    const int timeoutVal = timeoutMs;

    // 合并 stderr 到 stdout, 失败信息才能一并发给模型。
    // ⚠ 工作目录走 setWorkingDirectory, **不**拼 `cd "..." &&`: cmd.exe 对转义引号的
    //   解析跟 sh 完全不同(JZProcess 会给含空格的参数加引号), 拼进去轻则 cd 失败、
    //   重则整条命令被读错 —— 这也是旧 popen 那条链路的隐藏坑。
    // 解释器与旧 popen 保持一致: Windows = cmd.exe /c, POSIX = /bin/sh -c
    // (配了 terminal.shellProgram 就用它, 好跟终端行为对齐)。
    std::string run = cmd + " 2>&1";
    std::string shell;
    std::vector<std::string> args;
#ifdef _WIN32
    shell = m_config.shellProgram.empty() ? std::string("cmd.exe") : m_config.shellProgram;
    if (str_toLower(shell).find("cmd") == std::string::npos)
    {
        // 配了 bash 之类非 cmd 的 shell: 直接 `-c <命令>`(它们认 JZProcess 的 \" 转义)
        args.push_back("-c");
        args.push_back(run);
    }
    else
    {
        // ⭐ Windows 上要把命令**先写进临时 .cmd**, 再让 cmd.exe 只带一个脚本路径执行。
        //   直接 `cmd.exe /c "<含引号的命令>"` 会坏: JZProcess 按 argv 规则给整个参数加引号、
        //   并把内层 " 转义成 \" —— 而 **cmd.exe 不认 \"**, 内层引号会变成字面量。
        //   实测症状(agent_cli_test 的 hello_world 用例):
        //     python 收到带引号的路径 →
        //     can't open file 'C:\tmp\...\case_0\"C:\...\stdout_verify.py"' [Errno 22]
        //   (旧的 _popen 由 CRT 拼命令行, 所以没这个坑; 换成 JZProcess 后必须自己绕开。)
        std::string dir = tempDirPath();
        std::string path = dir + "/jz_verify_" + std::to_string(JZPlatform::getTickCountMs()) + "_" +
                           std::to_string(gen) + ".cmd";
        JZFile script;
        std::string body = "@echo off\r\n" + run + "\r\n";
        if (script.open(path, "wb"))
        {
            script.write(body.data(), (int)body.size());
            script.flush();
            script.close();
            m_verifyScript = path;
            args.push_back("/c");
            args.push_back(path);   // 只有一个干净的路径参数(内层引号全在脚本内容里)
        }
        else
        {
            // 写不出临时脚本(权限/磁盘): 退回直传, 至少能跑无引号的命令
            jzWarning() << "验证命令的临时脚本写不出来, 退回直接 -c 传参:" << path;
            args.push_back("/c");
            args.push_back(run);
        }
    }
#else
    shell = m_config.shellProgram.empty() ? std::string("/bin/sh") : m_config.shellProgram;
    args.push_back("-c");
    args.push_back(run);   // POSIX 下 argv 是直传(没有 cmd 那套引号规则)
#endif

    JZProcess *proc = new JZProcess(this);
    proc->setWorkingDirectory(cwd);
    m_verifyProc = proc;
    m_verifyRunning = true;

    auto collect = [this, proc]()
    {
        m_verifyOut += bytesToText(proc->readAllStandardOutput());
        m_verifyOut += bytesToText(proc->readAllStandardError());
        // 累积上限: 验证输出几十 KB 足够定位问题, 别让它把内存/JSON 回包撑爆
        if (m_verifyOut.size() > kMaxVerifyBytes * 2)
            m_verifyOut.resize(kMaxVerifyBytes);
    };
    connect(&proc->sigReadyReadStandardOutput, collect);
    connect(&proc->sigReadyReadStandardError, collect);
    connect(&proc->sigFinished, [this, proc, gen, collect](int exitCode, JZProcess::ExitStatus)
            {
                if (gen != m_verifyGen)
                    return;   // 上一轮验证的尾巴(已被 kill/replace), 丢掉
                collect();
                finishVerify(exitCode);
            });
    connect(&proc->sigErrorOccurred, [this, proc, gen](JZProcess::ProcessError)
            {
                if (gen != m_verifyGen)
                    return;
                // 起不来(解释器/脚本不存在): 与 popen 失败同义 —— 报 -1 + 可读原因,
                // 让"验证命令本身跑不起来"这条信息照样能回到模型(见 onVerifyDone 的 127/9009 处理)
                m_verifyOut = "无法启动验证命令: " + m_verifyCmd;
                finishVerify(-1);
            });

    if (!proc->start(shell, args))
        return;   // sigErrorOccurred 已经走过 finishVerify(它会 deleteLater)

    // 超时: 到点连**整棵进程树**一起杀(JZProcess 的作业对象/进程组)。
    // ⚠ Windows 以前完全不包超时(旧实现是 #ifndef _WIN32 的 timeout 包装) ⇒ 验证命令
    //   不退出就永久卡死; 现在两个平台都走这条真超时。
    if (timeoutVal > 0)
    {
        JZTimer::singleShot(timeoutVal, [this, gen]()
                             {
                                 if (gen != m_verifyGen || !m_verifyRunning)
                                     return;
                                 m_verifyTimedOut = true;
                                 if (m_verifyProc)
                                     m_verifyProc->killProcess();   // 同步收尾 -> finishVerify
                             });
    }
}

void JZAgentCli::killVerify()
{
    if (!m_verifyRunning)
        return;
    // 主动停: 结果已经没人要了(任务被重置/收尾), 所以先把分发清掉再杀
    m_verifyCb = nullptr;
    m_verifyRunning = false;
    m_verifyGen++;   // 让在途的超时定时器与信号都作废
    JZProcess *proc = m_verifyProc;
    m_verifyProc = nullptr;
    if (proc)
    {
        proc->killProcess();   // 同步收尾: 此刻 finishVerify 会因为 m_verifyRunning=false 直接返回
        proc->deleteLater();
    }
    if (!m_verifyScript.empty())
    {
        JZFile::remove(m_verifyScript);
        m_verifyScript.clear();
    }
}

// 一次验证结束(自然退出 / 超时被杀 / 起不来): 截断输出 -> 回调
void JZAgentCli::finishVerify(int exitCode)
{
    if (!m_verifyRunning)
        return;
    m_verifyRunning = false;

    JZProcess *proc = m_verifyProc;
    m_verifyProc = nullptr;
    if (proc)
        proc->deleteLater();

    if (!m_verifyScript.empty())
    {
        JZFile::remove(m_verifyScript);   // Windows 的临时包装脚本: 用完就删
        m_verifyScript.clear();
    }

    std::string out = m_verifyOut;
    if (m_verifyTimedOut)
    {
        // 带个 ASCII 标记: 脚本/冒烟按它判定("验证命令超时"是中文, 日志编码不一定对得上)
        out += "\n...[verify-timeout] 验证命令超时(" + std::to_string(m_verifyTimeoutMs) +
               " ms), 进程已被强制终止";
        exitCode = -1;
    }
    if (out.size() > kMaxVerifyBytes)
    {
        size_t total = out.size();
        out = out.substr(0, kMaxVerifyBytes) + "\n...(验证输出已截断, 共 " +
              std::to_string(total) + " 字节)";
    }

    std::function<void(int, const std::string &)> cb = m_verifyCb;
    m_verifyCb = nullptr;
    if (cb)
        cb(exitCode, out);   // 回调里可能再起一次验证 / 收尾, 放最后
}

// 喂回给模型的验证输出: 只留头尾(错误栈/断言通常在两端), 中间省略。
// 完整输出可能几十 KB, 整段贴回上下文很容易把 token 预算顶爆(顶爆只能靠压缩救, 成功率更低);
// 回包给界面/客户端的仍是完整输出(m_lastVerifyOutput)。
static std::string clipForModel(const std::string &out)
{
    const size_t kHead = 4096;
    const size_t kTail = 4096;
    if (out.size() <= kHead + kTail)
        return out;
    std::string r = out.substr(0, kHead);
    r += "\n...(中间省略 " + std::to_string(out.size() - kHead - kTail) + " 字节, 需要时用工具自己去看)...\n";
    r += out.substr(out.size() - kTail);
    return r;
}

void JZAgentCli::onLoopFinished()
{
    // 1) 无验证命令 = 无验证: 没有判定标准, 直接按"通过"收尾。
    //    agent 自身的报错只当提示带回去(网页显示成一行提示), 不把整个任务判成"失败"——
    //    任务本身没有可失败的验收项, 失败徽标只会误导(模型答完还在闲聊/轮次用尽很常见)。
    if (m_verifyCmd.empty())
    {
        respondTask("success", m_loopRound, m_accumReply, "", m_failed ? m_lastError : "");
        endLoop();
        return;
    }

    // 2) agent 这一轮自身报错(错误重试预算已用完 / 这条错误不值得重试): 验证循环救不了它 ——
    //    模型这会儿连请求都发不出去, 再喂回只是重复空转(白烧轮次)。但先给它一次机会:
    //    agent 之前几轮的改动可能已经让验证脚本通过了, 只是最后一条消息没取到 ——
    //    所以跑一次验证: 通过就算成功; 不通过就按 agent 错误收尾。
    //    (验证循环负责的是另一类失败: 模型能干活, 但结果不对。)
    if (m_failed)
    {
        runVerifyAsync(m_verifyCmd, m_verifyTimeoutMs, m_config.rootDir,
                       [this](int code, const std::string &out)
                       {
                           if (!m_loopActive)
                               return;   // 验证途中任务被重置了
                           if (code == 0)
                               respondTask("success", m_loopRound, m_accumReply, out, "");
                           else
                               respondTask("failed", m_loopRound, m_accumReply, out,
                                           m_lastError.empty() ? std::string("agent 执行失败")
                                                               : m_lastError);
                           endLoop();
                       });
        return;
    }

    // 3) 运行验证脚本(退出码 0 通过, 1 失败)。**异步**: 起进程后立刻返回, 结果经 onVerifyDone
    //    回到主线程 —— 期间事件循环照常转(SSE 心跳/网页请求/工具回调都不会被卡住)。
    runVerifyAsync(m_verifyCmd, m_verifyTimeoutMs, m_config.rootDir,
                   [this](int code, const std::string &out) { onVerifyDone(code, out); });
}

// 验证结果的分发(正常路径): SSE 事件 -> 通过/失败 -> 超过轮次 -> 把输出喂回 agent 续跑
void JZAgentCli::onVerifyDone(int exitCode, const std::string &out)
{
    if (!m_loopActive)
        return;   // 验证途中任务被重置/取消了

    m_lastVerifyOutput = out;

    Json::Value ve;
    ve["round"] = m_loopRound;
    ve["exit_code"] = exitCode;
    ve["output"] = out;
    ve["passed"] = (exitCode == 0);
    ve["max_rounds"] = m_maxLoopRounds;
    pushEvent("verify", ve);

    if (exitCode == 0)
    {
        respondTask("success", m_loopRound, m_accumReply, out, "");
        endLoop();
        return;
    }

    // 4) 失败: 超过最大轮次则放弃
    m_loopRound++;
    if (m_loopRound >= m_maxLoopRounds)
    {
        respondTask("failed", m_loopRound, m_accumReply, out,
                    "验证脚本在达到最大轮次(" + std::to_string(m_maxLoopRounds) + ")后仍失败");
        endLoop();
        return;
    }

    // 5) 把验证输出喂回 agent, 自动续跑
    //    先看看这一轮的失败是不是和上一轮一模一样(原地打转: 同样的改动再做一次还是会失败)
    std::string sig = std::to_string(exitCode) + "|" + out;
    if (sig == m_lastFeedbackSig)
        m_sameFailCount++;
    else
    {
        m_lastFeedbackSig = sig;
        m_sameFailCount = 1;
    }

    std::string feedback = "验证脚本执行失败(退出码 " + std::to_string(exitCode) + ")。"
        "其输出如下:\n```\n" + clipForModel(out) + "\n```\n"
        "请根据以上输出定位并修复问题, 然后我会重新运行验证脚本。只做必要的修改, 不要做无关改动。";
    // 9009(cmd: 不是内部或外部命令) / 127,126(sh: 命令未找到/不可执行):
    // 这是"验证命令本身跑不起来", 不是"测试没通过" —— 得让模型知道, 否则它会一直去改被测代码。
    if (exitCode == 9009 || exitCode == 127 || exitCode == 126)
        feedback += "\n注意: 这个退出码说明**验证命令本身**没找到或不能执行(不是测试没通过)。"
                    "请先把这条命令依赖的程序/脚本准备好, 或换成一条确实能跑的命令。";
    if (m_sameFailCount >= 3)
        feedback += "\n注意: 你已经连续 " + std::to_string(m_sameFailCount) +
                    " 次拿到完全相同的失败输出, 说明前面的做法没有生效。这一轮请先换做法: "
                    "用 read_file 读真正相关的文件、先把问题复现出来再改, 不要重复上一轮已经做过的修改。";
    m_accumReply.clear();
    m_thinkLogged = false;
    m_failed = false;
    m_cancelled = false;
    m_lastError.clear();
    agentLog("input(continue): " + logTrunc(feedback));
    m_agent->request(feedback);   // 重新驱动 agent; 结束后再次进入 onLoopFinished
}

// ---------------------------------------------------------------------------
// 最近任务历史
//   长程任务跑几十分钟很常见, 网页不可能一直开着 —— 服务端记住最近若干次请求与它们的结果,
//   新连接(SSE hello 帧)或 GET /info 时以 recent 数组吐出去, 界面据此把历史卡片补回来。
//   只存"摘要"(请求 / 结论 / 最后一次验证输出), 不存逐轮过程 —— 那是流式事件该干的事。
// ---------------------------------------------------------------------------
int64_t JZAgentCli::nowMs()
{
    return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

void JZAgentCli::pushHistory(const std::string &prompt, const std::string &verifyCmd)
{
    JZCliHistoryItem it;
    it.seq = ++m_historySeq;
    it.prompt = prompt;
    it.verifyCmd = verifyCmd;
    it.status = "running";
    it.startMs = nowMs();
    m_history.push_back(it);

    while ((int)m_history.size() > kHistoryMax)
        m_history.erase(m_history.begin());   // 最旧的先走
}

void JZAgentCli::finishHistory(const std::string &status, int rounds, const std::string &reply,
                               const std::string &verifyOut, const std::string &err)
{
    // 回填"最后一条还没收尾的"(正常就是刚跑完这条); 极端情况(如重置时才回包)没有就补一条
    for (int i = (int)m_history.size() - 1; i >= 0; --i)
    {
        if (m_history[i].status != "running")
            continue;
        JZCliHistoryItem &it = m_history[i];
        it.status = status;
        it.rounds = rounds;
        it.reply = reply;
        it.verifyOut = verifyOut;
        it.error = err;
        it.endMs = nowMs();
        return;
    }

    pushHistory(m_currentPrompt, m_verifyCmd);
    m_history.back().status = status;
    m_history.back().rounds = rounds;
    m_history.back().reply = reply;
    m_history.back().verifyOut = verifyOut;
    m_history.back().error = err;
    m_history.back().endMs = nowMs();
}

Json::Value JZAgentCli::recentJson() const
{
    Json::Value arr(Json::arrayValue);
    for (int i = (int)m_history.size() - 1; i >= 0; --i)   // 最近在前
    {
        const JZCliHistoryItem &it = m_history[i];
        Json::Value o;
        o["seq"] = it.seq;
        o["prompt"] = it.prompt;
        o["verify_cmd"] = it.verifyCmd;
        o["status"] = it.status;
        o["verify_rounds"] = it.rounds;
        o["agent_reply"] = it.reply;
        o["last_verify_output"] = it.verifyOut;
        if (!it.error.empty())
            o["error"] = it.error;
        o["start_ms"] = (Json::Int64)it.startMs;
        o["end_ms"] = (Json::Int64)it.endMs;
        arr.append(o);
    }
    return arr;
}

void JZAgentCli::respondTask(const std::string &status, int rounds, const std::string &reply,
                             const std::string &verifyOut, const std::string &err)
{
    Json::Value r;
    r["status"] = status;
    r["verify_rounds"] = rounds;
    r["retries"] = m_errorRetries + m_fixRetries;   // 本次任务用掉的重试次数(两档合计)
    r["retries_transient"] = m_errorRetries;        // 其中"继续"档
    r["retries_fix"] = m_fixRetries;                // 其中"改一版"档
    r["agent_reply"] = reply;
    r["last_verify_output"] = verifyOut;
    if (!err.empty())
        r["error"] = err;
    // 先推事件再回包: 提交方可能已经断开(见 onClientDestroyed), 那样只有 SSE 订阅者
    // (重连上来的网页)还能看到这次收尾 —— 它是那时唯一的结果出口。
    pushEvent("task_end", r);
    m_lastResult = r;              // 留一份: 断连期间跑完的任务, 重连时靠 hello 帧补给界面
    m_hasLastResult = true;
    finishHistory(status, rounds, reply, verifyOut, err);   // 归档进"最近任务"

    if (!m_respond)   // 客户端已断开 / 不应回包
        return;
    auto cb = std::move(m_respond);
    cb(r);
}

void JZAgentCli::endLoop()
{
    // 任务收尾: 在跑的验证进程先停掉(重置/取消时它还可能在跑; 正常路径下此时已经没有在跑的)
    killVerify();

    // 停掉这轮留下的后台命令(agent 起的服务之类)。
    // 后台任务没有超时、也不会自己结束, 而 agent 这会儿已经不管它们了 —— 不清掉就会跨任务
    // 残留(典型后果: 端口还占着, 下一个用例的服务起不来)。
    if (m_terminal)
    {
        int killed = m_terminal->cancelAll();
        if (killed > 0)
        {
            Json::Value e;
            e["killed"] = killed;
            pushEvent("terminal_cleanup", e);
        }
    }

    m_loopActive = false;
    m_busy = false;
    m_pendingClient = nullptr;
    m_respond = nullptr;
    m_verifyCmd.clear();
}

void JZAgentCli::onReset(JZHttpServerClient *client, const JZHttpRequest &req)
{
    if (!checkToken(req))
    {
        sendJson(client, 401, Json::objectValue);
        return;
    }
    if (!m_agent)
    {
        Json::Value e; e["error"] = "agent not initialized";
        sendJson(client, 500, e);
        return;
    }

    Json::Value root;
    Json::Reader reader;
    std::string scope = "task";
    if (reader.parse(std::string(req.body().data(), req.body().size()), root) && root.isObject())
        scope = root.get("scope", "task").asString();

    bool cancelRunning = (scope == "task");
    bool taskCancelled = false;

    // 清空历史: 连服务端记的"最近任务"一起清(否则网页一刷新它们又回来了)
    if (scope == "history")
    {
        m_history.clear();
        m_hasLastResult = false;
        m_lastResult = Json::nullValue;
    }

    // 有任务在跑: 先给调用方一个明确结果(status:"cancelled"), 再停 agent
    if (cancelRunning && m_loopActive)
    {
        m_loopActive = false;   // stop() 引发的 sigFinished 不再驱动验证循环
        m_busy = false;
        respondTask("cancelled", m_loopRound, m_accumReply, m_lastVerifyOutput, "任务已被重置");
        endLoop();
        taskCancelled = true;
    }

    resetSession(cancelRunning);

    Json::Value d;
    d["scope"] = scope;
    d["cancelled"] = taskCancelled;
    pushEvent("reset", d);

    Json::Value r;
    r["status"] = "ok";
    r["scope"] = scope;
    r["cancelled"] = taskCancelled;
    sendJson(client, 200, r);
}

void JZAgentCli::resetSession(bool cancelRunning)
{
    if (!m_agent)
        return;

    // stop() = llmError → llmEnd: 同步回到 Idle(并取消在途工具), 之后才允许 reset()
    if (cancelRunning)
        m_agent->stop();
    m_agent->reset();          // 仅 Idle 允许: 清对话历史 / 用量 / 待发请求
    m_sessionStarted = false;  // 下次提交按"新会话"处理
    m_loopRound = 0;
    m_accumReply.clear();
    m_thinkLogged = false;
    m_lastVerifyOutput.clear();
    m_currentPrompt.clear();
    m_accumThink.clear();
    m_thinkLogged = false;
    m_toolSnaps.clear();
    m_failed = false;
    m_cancelled = false;
    m_lastError.clear();
    m_lastErrorCode = JZAgent::None;
    m_errorRetries = 0;
    m_fixRetries = 0;
    m_retryPending = false;
    m_lastFeedbackSig.clear();
    m_sameFailCount = 0;
    m_loopEpoch++;             // 重置: 作废在途的延时重试
}

void JZAgentCli::onClientDestroyed(JZObject *obj)
{
    // 提交方(POST /task 的那条连接)在任务跑完前断开: 只是"没人收回包"了, 任务继续跑完。
    //   典型场景是网页刷新/关页面 —— 浏览器会 abort 掉那条 /task 请求, 这种断连不该把在跑的
    //   任务杀掉(改了一半的工作就丢了)。结果仍会经 SSE 推给在线订阅者(task_end / verify ...),
    //   网页重连时也能用 state 快照接着看进行中的任务。
    //   ⚠ 所以这里只丢回包通道(避免写野指针), 刻意不动 m_loopActive / 不 stop agent。
    //   真要中止任务走 POST /reset(那是用户明确的操作)。
    if (obj != m_pendingClient)
        return;   // 不是当前在途任务的那条连接(上一个任务早已回包, 连接延后析构): 与本任务无关

    m_pendingClient = nullptr;
    m_respond = nullptr;   // 断开即不再回包; 任务本身照跑, 结束由 endLoop() 复位状态
}

void JZAgentCli::onListScripts(JZHttpServerClient *client, const JZHttpRequest &req)
{
    (void)req;
    static const char *kExts[] = {".py", ".sh", ".bat", ".cmd", ".js", ".ps1", nullptr};
    std::vector<std::string> filters;
    for (int i = 0; kExts[i]; ++i)
        filters.push_back(kExts[i]);
    JZDir dir(m_config.rootDir);
    std::vector<std::string> files = dir.entryList(filters, JZDir::Files | JZDir::NoDotAndDotDot);

    Json::Value arr(Json::arrayValue);
    for (const auto &f : files)
        arr.append(f);
    Json::Value r;
    r["scripts"] = arr;
    sendJson(client, 200, r);
}

string JZAgentCli::extractUserText(const Json::Value &params)
{
    const Json::Value &msgs = params["messages"];
    if (msgs.isArray())
    {
        string out;
        for (Json::ArrayIndex i = 0; i < msgs.size(); ++i)
        {
            const Json::Value &m = msgs[i];
            string role = m.get("role", "").asString();
            const Json::Value &c = m["content"];
            if (role != "user")
                continue;
            if (c.isString())
            {
                out += c.asString();
            }
            else if (c.isArray())
            {
                for (Json::ArrayIndex j = 0; j < c.size(); ++j)
                {
                    const Json::Value &p = c[j];
                    if (p.isObject() && p["type"].asString() == "text")
                        out += p["text"].asString();
                    else if (p.isString())
                        out += p.asString();
                }
            }
        }
        if (!out.empty())
            return out;
    }
    // 退化: 直接给 prompt / text 字段
    if (params.isMember("prompt") && params["prompt"].isString())
        return params["prompt"].asString();
    if (params.isMember("text") && params["text"].isString())
        return params["text"].asString();
    return string();
}

// 收集图片附件(多模态)。两种写法都收 —— 与 OpenAI 的 messages 一致, 网页/脚本可直接搬:
//   "images": ["data:image/png;base64,...", "http://host/a.png"]     (单张也允许直接给字符串)
//   "messages": [{role:"user", content:[{type:"text"},{type:"image_url",image_url:{url}}]}]
// 只认 data: URL 与 http(s) 链接(别的形态喂给模型只会拿到 400)。
// ⚠ 上限是硬约束(kMaxTaskImages / kMaxImageChars): 超了就报错, 不静默截断 —— 否则
//   提交方以为图发过去了、模型却看不到, 排查起来很费劲。
std::vector<std::string> JZAgentCli::extractImages(const Json::Value &params, string &error)
{
    std::vector<std::string> out;
    string bad;
    size_t total = 0;

    auto add = [&](const Json::Value &v) {
        if (!v.isString())
            return;
        string url = v.asString();
        if (url.empty())
            return;
        if ((int)url.size() > kMaxImageChars)
        {
            if (bad.empty()) bad = "单张图片过大(超过 " + to_string(kMaxImageChars) + " 字符)";
            return;
        }
        if (total + url.size() > (size_t)kMaxImagesTotalChars)
        {
            if (bad.empty()) bad = "图片合计过大(超过 " + to_string(kMaxImagesTotalChars) + " 字符)";
            return;
        }
        string head = str_toLower(url.substr(0, 5));
        if (head.compare(0, 5, "data:") != 0 && head.compare(0, 4, "http") != 0)
        {
            if (bad.empty()) bad = "images 只接受 data: URL 或 http(s) 链接";
            return;
        }
        if ((int)out.size() >= kMaxTaskImages)
        {
            if (bad.empty()) bad = "图片数量过多(超过 " + to_string(kMaxTaskImages) + " 张)";
            return;
        }
        total += url.size();
        out.push_back(url);
    };

    const Json::Value &images = params["images"];
    if (images.isArray())
    {
        for (Json::ArrayIndex i = 0; i < images.size(); ++i)
            add(images[i]);
    }
    else if (images.isString())
    {
        add(images);
    }

    // messages 写法: 只取 user 消息 content 数组里的 image_url 部分(与 extractUserText 同一套遍历)
    const Json::Value &msgs = params["messages"];
    if (msgs.isArray())
    {
        for (Json::ArrayIndex i = 0; i < msgs.size(); ++i)
        {
            const Json::Value &m = msgs[i];
            if (m.get("role", "").asString() != "user")
                continue;
            const Json::Value &c = m["content"];
            if (!c.isArray())
                continue;
            for (Json::ArrayIndex j = 0; j < c.size(); ++j)
            {
                const Json::Value &p = c[j];
                if (!p.isObject() || p["type"].asString() != "image_url")
                    continue;
                const Json::Value &iu = p["image_url"];
                if (iu.isObject())
                    add(iu.get("url", ""));
                else if (iu.isString())
                    add(iu);
            }
        }
    }

    // 一张都没收进来但确实有毛病 -> 报错; 收了部分(超数量)也报错, 让提交方知道被截断了
    if (!bad.empty())
        error = bad;
    return out;
}

// ==========================================================================
// JZAgent 信号
// ==========================================================================

void JZAgentCli::onReply(const std::string &text)
{
    // 回复开始 = 思考结束: 先把整条 think 落盘(一条 think 一条, 不切块), 超长折叠头尾
    if (!m_thinkLogged && !m_accumThink.empty())
    {
        agentLog(logChunk("think", m_accumThink, m_accumThink.size()));
        m_thinkLogged = true;
    }

    m_accumReply += text;   // 累积本轮最终回复

    if (hasEventConsumer())
    {
        Json::Value d;
        d["text"] = text;
        pushEvent("reply", d);
    }
}

void JZAgentCli::onThinking(const std::string &text)
{
    m_accumThink += text;          // 累积, 供连接快照(与回包无关); 完整 think 在结束时整条落盘

    if (!hasEventConsumer())
        return;

    Json::Value d;
    d["text"] = text;
    pushEvent("thinking", d);
}

void JZAgentCli::onToolCall(const ToolCall &call)
{
    JZCliToolSnap snap;
    snap.id = call.id;
    snap.name = call.function_name;
    snap.args = call.function_args;
    m_toolSnaps.push_back(snap);   // 记录, 供连接快照(与合并日志)

    if (!hasEventConsumer())
        return;

    Json::Value d;
    d["id"] = call.id;
    d["name"] = call.function_name;
    d["args"] = call.function_args;
    pushEvent("tool_call", d);
}

void JZAgentCli::onToolResult(const ToolCallResult &result)
{
    // 一次调用合并为一条: tool_call + tool_result(结果回来时才写, 不单独刷 tool_call)
    for (auto &t : m_toolSnaps)
    {
        if (t.id == result.id)
        {
            t.hasResult = true;
            t.ok = result.isOk;
            t.result = result.result;
            agentLog("tool: " + t.name + " args=" + logTrunc(t.args)
                     + " => " + std::string(t.ok ? "ok" : "fail") + " " + logTrunc(t.result));
            break;
        }
    }

    if (!hasEventConsumer())
        return;

    Json::Value d;
    d["id"] = result.id;
    d["ok"] = result.isOk;
    d["result"] = result.result;
    pushEvent("tool_result", d);
}

// ---------------------------------------------------------------------------
// 出错自动重试
//   瞬时错误(连接失败/超时/5xx)不该让整个任务直接判失败 —— 把错误原因告诉模型,
//   让它从中断处继续做。分类来自 JZAgent::errorCode(): llmError() 里先置分类再发
//   sigError, 所以 onError 里读到的就是本次错误的分类。
// ---------------------------------------------------------------------------

// "HTTP 402 错误: ..." -> 1=瞬时(原样重发有意义) / 2=可修复(原样重发必然再失败)。
// 408/409/425/429 是超时/冲突/限流, 归瞬时; 其余 4xx 是"我们发出去的东西有问题"
// (400 参数或工具调用 JSON 不合法、413/422 等), 归可修复: 把原因丢回模型让它改一版。
// 没有状态码(连接失败/超时等)按瞬时处理。
static int httpErrorMode(const std::string &err)
{
    if (err.compare(0, 5, "HTTP ") != 0)
        return 1;

    int code = atoi(err.c_str() + 5);
    if (code <= 0)
        return 1;
    if (code >= 500 || code == 408 || code == 409 || code == 425 || code == 429)
        return 1;
    return (code < 400) ? 1 : 2;
}

JZAgentCli::RetryMode JZAgentCli::classifyError(JZAgent::AgentError code, const std::string &err)
{
    switch (code)
    {
        case JZAgent::ConnectError:
        case JZAgent::TimeOutError:
        case JZAgent::NoError:          // 内部/解析类错误: 多半是瞬时协议问题
        case JZAgent::ToolCallError:
            return RetryTransient;
        case JZAgent::HttpError:
            return (httpErrorMode(err) == 2) ? RetryFix : RetryTransient;
        case JZAgent::MaxRoundError:
            // 轮次用尽只是"这一轮预算跑完": llmEnd() 已把轮次计数清零, 发条"继续"就能接着干
            // (总量仍受重试次数约束, 不会无限跑)。
            return RetryTransient;
        case JZAgent::MaxTokenError:
            // 上下文撑爆: 原样重发还是超, 但"让模型收着点"(别再整段贴文件/贴输出)常能救回来。
            return RetryFix;
        case JZAgent::LoopError:
            // 在原地打转(工具反复相同调用 / 思考回复复读), 已被循环检测中止: 原样重发
            // 必然接着打转, 但"换做法"通常能解 —— 归可修复(预算受 maxFixRetries 约束)。
            return RetryFix;
        default:                        // None / UserCancelError
            return RetryNone;
    }
}

// 重试时喂给模型的"接着干"消息: 带上出错原因, 让它从中断处继续 / 换个做法
std::string JZAgentCli::buildRetryPrompt(RetryMode mode, JZAgent::AgentError code,
                                         const std::string &err) const
{
    std::string e = err.empty() ? std::string("(无错误信息)") : err;
    if (e.size() > 300)
        e = e.substr(0, 300) + "…";

    if (mode == RetryFix)
    {
        // 循环检测中止: 唯一的解法是"别再重复" —— 笼统的"换做法"模型容易理解成
        // "把同样的命令再发一次"或"把那段话再写一遍", 所以这里两样都点名。
        if (code == JZAgent::LoopError)
            return "上一轮被中止了, 原因不是任务失败, 而是检测到你在原地打转:\n" + e + "\n"
                   "这一轮请**不要再重复**: 不要用完全相同的参数再调用同一个工具"
                   "(不要重复读同一个文件、不要重复跑同一条命令), 也不要把同样的内容"
                   "反复写出来。先换做法(换目标/换参数/换工具确认现状), "
                   "如果信息其实已经足够, 就直接给出最终结论。";

        return "上一次请求被服务端拒绝了: " + e + "\n"
               "这是第 " + std::to_string(m_fixRetries) + " 次修正尝试: 原样重发还会被拒, "
               "请换一个做法把当前任务做完 —— 若上一条工具调用的参数不是合法 JSON, 就重新发一个参数合法的调用; "
               "若上下文太长, 不要再整段贴文件/整段贴命令输出, 改用搜索定位到具体行再看; "
               "已经完成的部分不要重复做, 已经确认的结论直接沿用。";
    }

    return "上一次请求因错误中断: " + e + "\n"
           "这是一次自动重试, 请从中断处继续完成当前任务: 已经完成的部分不要重复做, "
           "已经确认的结论直接沿用; 若任务其实已经完成, 直接给出最终结论即可。";
}

// 记一次重试 -> 等 retryDelayMs -> 再 request 一条"继续/修正"消息(结束后重新进 onFinished 走验证)
void JZAgentCli::scheduleErrorRetry(RetryMode mode)
{
    int epoch = m_loopEpoch;              // 记下当前任务世代, 延时期间任务换掉就不再续跑
    bool fix = (mode == RetryFix);
    int next = (fix ? m_fixRetries : m_errorRetries) + 1;
    int budget = fix ? m_maxFixRetries : m_maxErrorRetries;
    std::string err = m_lastError;
    JZAgent::AgentError code = m_lastErrorCode;   // 延时期间可能被覆盖, 先记下来给提示文案用

    // 半截回复作废, 重试后重新累积(与验证失败续跑同一套语义); 成败等重试结果出来再定
    m_accumReply.clear();
    m_thinkLogged = false;
    m_failed = false;
    m_lastError.clear();

    Json::Value d;
    d["error"] = err;
    d["code"] = (int)m_lastErrorCode;
    d["mode"] = fix ? "fix" : "continue";    // fix = 让模型换做法; continue = 从中断处接着干
    d["retry"] = next;
    d["max_retries"] = budget;
    d["delay_ms"] = m_retryDelayMs;
    pushEvent("retry", d);

    jzWarning().nospace() << "agent 报错, 第 " << next << "/" << budget
                          << (fix ? " 次修正式重试" : " 次自动重试")
                          << "(" << m_retryDelayMs << "ms 后): " << err;

    auto fire = [this, epoch, err, mode, code]() {
        m_retryPending = false;
        // 等待期间被取消/重置/换了任务: 不再续跑(否则会把消息发进新任务的会话里)
        if (!m_loopActive || epoch != m_loopEpoch || !m_busy)
            return;
        if (!m_agent)
            return;

        if (mode == RetryFix)
            m_fixRetries++;
        else
            m_errorRetries++;
        std::string retryPrompt = buildRetryPrompt(mode, code, err);
        agentLog("input(retry/" + std::string(mode == RetryFix ? "fix" : "transient") + "): " + logTrunc(retryPrompt));
        m_agent->request(retryPrompt);
    };

    m_retryPending = true;
    if (m_retryDelayMs > 0)
        JZTimer::singleShot(m_retryDelayMs, this, fire);
    else
        fire();
}

void JZAgentCli::onError(const std::string &err)
{
    m_failed = true;
    m_lastError = err;
    m_lastErrorCode = m_agent ? m_agent->errorCode() : JZAgent::NoError;
    agentLog("error: " + logTrunc(err));

    RetryMode mode = classifyError(m_lastErrorCode, err);
    bool retryable = (mode == RetryTransient) ? (m_errorRetries < m_maxErrorRetries)
                   : (mode == RetryFix)       ? (m_fixRetries < m_maxFixRetries)
                                              : false;

    // sigError 实时推给网页: 任务中途出错(LLM 失败/拒答/取消等)不再只能等最后回包
    Json::Value d;
    d["text"] = err;
    d["round"] = m_loopRound;
    d["loop_active"] = m_loopActive;
    d["code"] = (int)m_lastErrorCode;    // 错误分类, 见 JZAgent::AgentError
    d["retry"] = (mode == RetryFix) ? m_fixRetries : m_errorRetries;   // 该档已用掉的重试次数
    d["retry_mode"] = (mode == RetryFix) ? "fix" : (mode == RetryTransient ? "continue" : "none");
    d["retryable"] = retryable;          // 还会自动重试吗(网页据此提示"将自动重试")
    pushEvent("error", d);
}

void JZAgentCli::onFinished()
{
    // 任务被取消 / 客户端断开时不跑验证
    if (!m_loopActive)
        return;

    // 本轮 LLM 生成结束: 整条落盘(一条 think / 一条 reply; think 若已写则不重复)
    if (!m_thinkLogged && !m_accumThink.empty())
    {
        agentLog(logChunk("think", m_accumThink, m_accumThink.size()));
        m_thinkLogged = true;
    }
    if (!m_accumReply.empty())
        agentLog(logChunk("reply", m_accumReply, m_accumReply.size()));

    // agent 报错: 在(分档的)重试预算内"接着发继续任务"/"让它改一版", 而不是直接判失败
    // (预算用完才走原流程: 有验证脚本就交给验证循环处理)
    if (m_failed)
    {
        RetryMode mode = classifyError(m_lastErrorCode, m_lastError);
        if ((mode == RetryTransient && m_errorRetries < m_maxErrorRetries)
            || (mode == RetryFix && m_fixRetries < m_maxFixRetries))
        {
            scheduleErrorRetry(mode);
            return;
        }
    }

    // agent 每结束一轮就跑验证, 失败则把输出喂回 agent 自动续跑
    onLoopFinished();
}
