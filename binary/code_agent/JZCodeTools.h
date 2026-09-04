#ifndef JZ_CODE_TOOLS_H_
#define JZ_CODE_TOOLS_H_

#include <map>
#include <string>
#include <vector>

#include "JZAgentTool.h"
#include "JZProcess.h"
#include "JZTimer.h"

// ------------------------------------------------------------------
// 工作区路径工具（网页端文件浏览复用同一套越界校验，避免两处规则不一致）
// ------------------------------------------------------------------
bool codeIsSubPath(const std::string &path, const std::string &root);

// 把用户传入的（相对/绝对）路径解析为工作区内的绝对路径，越界返回 false
bool codeResolvePath(const std::string &workspace, const std::string &input,
                     std::string &absPath, std::string &error);

// 网络搜索工具依赖 JZNet，前置声明避免头文件耦合
class JZNetworkAccessManager;
class JZNetworkReply;

// 路径规范化：统一分隔符、处理 . / .. 、保留盘符前缀。
// 注意它不解析 junction/symlink（与 JZFileInfo::weaklyCanonicalPath 一样只是字符串规整），
// 不能单独用作沙箱依据。
std::string codeNormalizePath(const std::string &path, bool *escaped = nullptr);

// 列出工作区某目录的直接子项（目录名以 '/' 结尾），rel 为空表示根目录
bool codeListDir(const std::string &workspace, const std::string &rel,
                 std::vector<std::string> &out, std::string &error);

// 读取工作区内文本文件（utf8 输出）
bool codeReadFile(const std::string &workspace, const std::string &rel,
                  std::string &text, std::string &error);

// ------------------------------------------------------------------
// 工具拒绝原因（结构化）
//   只用来让上层(CJZCodeAgent)区分"真的失败了"与"被策略拦了"，
//   从而对"反复被拦"这类卡死行为做统计与熔断。
//   普通的调用错误(参数缺失、文件过大等)用 None，由"重复调用"那道闸兜住。
// ------------------------------------------------------------------
enum class DenyReason
{
    None = 0,           // 非拒绝类失败
    PathOutside,        // 路径越界，不在工作区/授信区内
    NotFound,           // 文件/目录/程序不存在
    CommandDenied,      // 命令被策略禁止
    ProgramDenied,      // 程序不在允许执行的区域
    EnvChange,          // 会修改环境 / 下载安装
    EditFailed          // 编辑类失败(old_str 不匹配、匹配多处等)
};

// 拒绝原因的中文说明（用于日志与提示）
const char *denyReasonText(DenyReason reason);

// ------------------------------------------------------------------
// 所有代码工具的基类：
//   - 统一管理工作区根目录与路径越界校验
//   - 统一的结果回传（finish）
//   - 统一的调用/返回日志信号（sigToolLog），供网页端展示执行过程
// ------------------------------------------------------------------
class JZCodeTool : public JZAgentTool
{
public:
    JZCodeTool();
    virtual ~JZCodeTool();

    void setWorkspace(const std::string &path);
    const std::string &workspace() const;

    bool resolvePath(const std::string &input, std::string &absPath, std::string &error) const;

    static ParamSchema makeParam(const std::string &name, ParamType type, const std::string &desc);

jz_signals:
    // (类别: "call" / "result", 工具名, 文本, 拒绝原因)
    JZSignal<const std::string &, const std::string &, const std::string &, int> sigToolLog;

protected:
    void finish(const ToolCall &call, bool ok, const std::string &text,
                DenyReason deny = DenyReason::None);
    void emitLog(const std::string &type, const std::string &text,
                 DenyReason deny = DenyReason::None);

    std::string m_workspace;
};

// ------------------------------------------------------------------
// list_dir: 列出目录内容
// ------------------------------------------------------------------
class JZListDirTool : public JZCodeTool
{
public:
    JZListDirTool();
    virtual void call(const ToolCall &params) override;
};

// ------------------------------------------------------------------
// read_file: 读取文件（可按行号范围）
// ------------------------------------------------------------------
class JZReadFileTool : public JZCodeTool
{
public:
    JZReadFileTool();
    virtual void call(const ToolCall &params) override;
};

// ------------------------------------------------------------------
// search_code: 在工作区内搜索文本 / 正则
// ------------------------------------------------------------------
class JZSearchCodeTool : public JZCodeTool
{
public:
    JZSearchCodeTool();
    virtual void call(const ToolCall &params) override;
};

// ------------------------------------------------------------------
// modify_code: 修改（或创建）文件
//   old_str 为空  -> 用 new_str 覆盖整个文件（新建/重写）
//   old_str 非空  -> 精确替换，默认要求唯一匹配
// ------------------------------------------------------------------
class JZModifyCodeTool : public JZCodeTool
{
public:
    JZModifyCodeTool();
    virtual void call(const ToolCall &params) override;
};

// ------------------------------------------------------------------
// 进程运行器：管理工作区内的子进程（超时 kill + 输出回传）
//   派生类只负责决定"启动什么程序"，结果的收集、超时终止与回传由这里统一处理，
//   避免每个用到进程的工具各写一套。
// ------------------------------------------------------------------
class JZProcessRunner : public JZCodeTool
{
public:
    virtual ~JZProcessRunner();

    // 停止所有仍在运行的子进程（用户点"停止"或熔断时调用）
    void stopAllProcesses();

protected:
    struct Task
    {
        std::string id;
        JZProcess *proc = nullptr;
        JZTimer *timer = nullptr;
        int64_t startTime = 0;
        bool timeout = false;
    };

    // logText 是展示用的命令文本（与实际启动的命令行可能不同）
    // emitCallLog 为 false 表示调用方已经发过 "call" 日志（避免上层重复计数）
    bool runProcess(const ToolCall &call, const std::string &program,
                    const std::vector<std::string> &args, const std::string &workDir,
                    int timeoutMs, const std::string &logText, bool emitCallLog = true);

    void onProcFinished(JZProcess *proc, const std::string &id,
                        int exitCode, JZProcess::ExitStatus status);
    void removeTask(JZProcess *proc);

    std::vector<Task> m_tasks;
};

// ------------------------------------------------------------------
// 通用命令工具: 用 JZProcess 在工作区内执行一条外部命令
//   - 命令由大模型在 command 参数里给出，也可以配一条默认命令
//   - Windows 下统一通过 cmd /c 执行，因此支持 dir/echo 等内部命令，
//     以及管道(|)、重定向(>)、&& 串联、.bat/.cmd 等写法
//   - 支持 work_dir(限制在工作区内) 与 timeout，超时会被强制终止
// ------------------------------------------------------------------
class JZCommandTool : public JZProcessRunner
{
public:
    JZCommandTool();

    void setup(const std::string &name, const std::string &desc,
               const std::string &defaultCommand, int timeoutMs);
    void setDefaultCommand(const std::string &cmd);
    const std::string &defaultCommand() const;

    virtual void call(const ToolCall &params) override;

private:
    std::string m_defaultCommand;
    int m_timeoutMs = 120000;
};

// ------------------------------------------------------------------
// 虚拟终端的区域模型
//   Workspace  工作区，可读写
//   Trusted    授信目录（编译器 / SDK 等），默认只读
//   Outside    其余一切，一律拒绝
// ------------------------------------------------------------------
enum class Zone
{
    Outside = 0,
    Workspace,
    Trusted
};

// 授信目录：每个目录可单独配置是否允许写入（默认只读）
struct TrustedDir
{
    std::string path;
    bool writable = false;                  // 是否允许写入（默认只读）
};

struct ZoneConfig
{
    std::string workspace;                  // 工作区根（可读写）
    std::vector<TrustedDir> trustedDirs;    // 授信目录（默认只读，可单独标记可写）
};

const char *zoneText(Zone zone);

// 判定一个绝对路径属于哪个区域
Zone zoneOfPath(const ZoneConfig &cfg, const std::string &absPath);

// 解析路径并判定区域：相对路径基于 cwd（cwd 自身也在工作区/授信区内），越界返回 false
bool zoneResolvePath(const ZoneConfig &cfg, const std::string &cwd, const std::string &input,
                     std::string &absPath, Zone &zone, std::string &error);

// 写操作只允许落在工作区（与当前目录无关）
bool zoneCheckWrite(const ZoneConfig &cfg, const std::string &absPath, std::string &error);

// ------------------------------------------------------------------
// 命令规格表
//   shellTemplate 为空   -> 由 C++ 自己实现
//   shellTemplate 非空   -> 路径审查通过后按模板转发 cmd /c，如 "copy /Y {0} {1}"；
//                           {0}/{1} 填的是已规范化的绝对路径，flag 在这里固定死，
//                           不允许模型传入（del/copy 不加 /Q //Y 会等输入卡死）
// ------------------------------------------------------------------
enum class ArgKind { Literal, Path };
enum class Access { None, Read, Write };

struct ArgSpec
{
    ArgSpec() = default;
    ArgSpec(ArgKind k, Access a) : kind(k), access(a) {}
    ArgKind kind = ArgKind::Literal;
    Access access = Access::None;
};

struct CommandSpec
{
    std::string name;
    std::string usage;
    std::vector<ArgSpec> args;
    bool allowRecursive = false;      // 是否接受 -r
    std::string shellTemplate;        // 为空=自己实现；非空=路径审查后转发 shell
                                      // （按平台在构造时填 cmd /c 或 sh -c 的语法）
    std::string shellTemplateDir;     // 目标是目录时改用此模板（rm -r 走 rd /S /Q）
};

// 严格词法解析：支持引号包裹含空格的参数；引号外的 shell 控制字符一律拒绝。
// 不支持管道 / 重定向 / 命令串联 —— 它们会让"一条命令"变成"多条命令"从而绕过参数审查。
bool splitCommandStrict(const std::string &cmd, std::vector<std::string> &argv, std::string &error);

// ------------------------------------------------------------------
// 外部程序白名单
//   定位是"副作用管控"：不让 agent 擅自安装东西、下载东西、改系统环境。
//   不是安全边界 —— 白名单里有编译器/解释器就等于任意代码执行，拦不住，
//   这里只做收敛与防误操作，真正的边界只能是沙箱。
// ------------------------------------------------------------------
enum class CmdCategory
{
    Tool = 0,           // 编译/构建/测试：默认允许
    PackageManager,     // pip/npm/conda 等：默认拒绝，设置里可开
    Network,            // curl/wget 等：默认拒绝，设置里可开
    SystemAdmin         // reg/sc/net/setx 等：永不放行
};

struct ProgramSpec
{
    std::string name;
    std::vector<std::string> exeNames;      // 可执行文件全名（同时兼容有无 .exe）
    CmdCategory category = CmdCategory::Tool;
    std::vector<std::string> denyFlags;     // 禁止出现的参数（前缀匹配）
    std::vector<std::string> denyModules;   // 配合 -m：挡掉 python -m pip 这类
};

struct ProgramPolicy
{
    bool allowPackageManager = false;   // 默认关：不要安装/升级依赖
    bool allowNetwork = false;          // 默认关：不要下载
    // SystemAdmin 永不放行，不提供开关
};

// 显式指定的可信程序：用全路径直接定位，优先于在授信目录里按名搜索。
// name 需与白名单 ProgramSpec 的 name 对应（如 cmake / python）。
struct TrustedProgram
{
    std::string name;   // 程序名（小写，与 ProgramSpec 的 name 对应）
    std::string path;   // 可执行文件全路径
};

// ------------------------------------------------------------------
// terminal: 虚拟终端
//   模型像在终端里敲命令一样使用它，但命令由我们自己解释，不经通用 shell。
//   支持多个命名会话，每个会话独立维护当前目录（cd 的效果会保留到后续命令）。
// ------------------------------------------------------------------
class JZVirtualTerminal : public JZProcessRunner
{
public:
    JZVirtualTerminal();

    void setZoneConfig(const ZoneConfig &cfg);
    const ZoneConfig &zoneConfig() const;

    void setProgramPolicy(const ProgramPolicy &policy);
    const ProgramPolicy &programPolicy() const;

    // 显式指定的可信程序（全路径）：优先于在授信目录里按名搜索
    void setTrustedPrograms(const std::vector<TrustedProgram> &progs);
    const std::map<std::string, std::string> &trustedPrograms() const;

    // 当前实际可用的程序名（在授信目录里真实存在的），供提示词动态生成
    std::vector<std::string> availablePrograms() const;

    virtual void call(const ToolCall &params) override;

protected:
    struct Session
    {
        std::string cwd;    // 规范化后的绝对目录
    };

    void addProgram(const std::string &name, std::vector<std::string> exeNames,
                    CmdCategory category,
                    std::vector<std::string> denyFlags = std::vector<std::string>(),
                    std::vector<std::string> denyModules = std::vector<std::string>());
    // 在授信目录里定位程序；返回 false 表示没装或没配授信目录
    bool findProgram(const std::string &name, const std::vector<std::string> &exeNames, std::string &abs) const;

    Session &session(const std::string &name);
    std::string relToWorkspace(const std::string &abs) const;

    // 内建命令（自己实现）
    void runHelp(const ToolCall &call, const std::vector<std::string> &argv);
    void runPwd(const ToolCall &call, const std::vector<std::string> &argv, Session &s);
    void runCd(const ToolCall &call, const std::vector<std::string> &argv, Session &s);
    void runLs(const ToolCall &call, const std::vector<std::string> &argv, Session &s);
    void runCat(const ToolCall &call, const std::vector<std::string> &argv, Session &s);

    // 转发类命令（路径审查通过后按 shellTemplate 走 cmd /c）
    void runShellCommand(const ToolCall &call, const std::string &name,
                         const CommandSpec &spec, const std::vector<std::string> &argv, Session &s);
    // 外部程序：白名单 + 绝对路径化 + 不经 shell
    void runExternalProgram(const ToolCall &call, const std::vector<std::string> &argv, Session &s);
    bool checkCategory(CmdCategory cat, std::string &error) const;
    // 解析出一个路径参数；writeAccess 为真时额外要求落在工作区
    bool resolveArg(const std::vector<std::string> &argv, int index, const Session &s,
                    bool writeAccess, std::string &abs, std::string &error, DenyReason &deny);
    // 拼进 cmd 命令行前的检查：拒绝引号等会破坏命令行的字符
    bool checkShellSafe(const std::string &path, std::string &error);

    ZoneConfig m_zone;
    std::map<std::string, Session> m_sessions;
    std::map<std::string, CommandSpec> m_specs;
    std::map<std::string, ProgramSpec> m_programs;
    std::map<std::string, std::string> m_trustedPrograms;   // 小写 name -> 全路径
    ProgramPolicy m_policy;
};

// ------------------------------------------------------------------
// web_search: 联网搜索
//   用 JZNetworkAccessManager 发起 HTTP GET 到一个返回 JSON 的搜索接口，
//   解析出若干条结果（标题 / 链接 / 摘要）返回给模型。
//   搜索接口通过 setSearchUrlTemplate 配置，模板含 {q} 占位符（会被 URL 编码后的查询词替换）。
//   未配置时使用内置默认（DuckDuckGo 即时答案 JSON 接口，无需 API Key）。
//   网络访问管理器由调用方通过 setNetworkAccessManager 注入（所有网络工具共享同一个，
//   代理也由调用方在该管理器上统一设置一次），工具本身不持有所有权、不单独配置代理。
// ------------------------------------------------------------------
class JZWebSearchTool : public JZCodeTool
{
public:
    JZWebSearchTool();

    void setSearchUrlTemplate(const std::string &urlTemplate);
    // 注入共享的网络访问管理器（非拥有）；代理由调用方在管理器上统一设置
    void setNetworkAccessManager(JZNetworkAccessManager *net);

    virtual void call(const ToolCall &params) override;

private:
    void handleReply(JZNetworkReply *reply, const ToolCall &call, int num);
    void parseSearch(const std::string &body, const ToolCall &call, int num);

    JZNetworkAccessManager *m_net = nullptr;   // 非拥有，来自外部注入
    JZNetworkReply *m_reply = nullptr;
    std::string m_searchUrlTemplate;
};

// ------------------------------------------------------------------
// web_fetch: 抓取网页正文
//   给定 URL，GET 后把 HTML 转成纯文本返回（截断到 max_chars），
//   与 web_search 配合：search 给摘要 + 链接，fetch 取某条链接的正文。
//   仅允许 http/https；网络访问管理器由调用方注入（与 web_search 共享同一个，
//   代理统一在管理器上设置一次），工具本身不持有所有权、不单独配置代理。
// ------------------------------------------------------------------
class JZWebFetchTool : public JZCodeTool
{
public:
    JZWebFetchTool();

    // 注入共享的网络访问管理器（非拥有）；代理由调用方在管理器上统一设置
    void setNetworkAccessManager(JZNetworkAccessManager *net);

    virtual void call(const ToolCall &params) override;

private:
    void handleReply(JZNetworkReply *reply, const ToolCall &call, int maxChars);

    JZNetworkAccessManager *m_net = nullptr;   // 非拥有，来自外部注入
    JZNetworkReply *m_reply = nullptr;
};

#endif // !JZ_CODE_TOOLS_H_
