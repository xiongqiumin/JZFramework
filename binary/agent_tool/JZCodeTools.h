#ifndef JZ_CODE_TOOLS_H_
#define JZ_CODE_TOOLS_H_

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "JZAgentTool.h"
#include "JZProcess.h"
#include "JZTimer.h"
#include "JZCommandLineParser.h"
#include "JZVirtualTerminalConfig.h"   // Zone / TrustedDir / ZoneConfig / JZVirtualTerminalConfig

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

// bash(MSYS/Git-Bash) 风格路径 -> Windows 路径的翻译在 **JZVirtualTerminalConfig.h**
// （codeBashPathToWindows / codeLooksLikeBashPath / zoneTempDirs / zoneCheckPath）——
// 权限判定与文件工具**共用那一份**, 免得再出现"终端里有效、文件工具里恒不存在"。

// 判断路径是否指向文本文件（采样前缀做 UTF-8 合法性判定，空文件当作文本）
bool isTextFile(const std::string &path);

// 列出工作区某目录的直接子项（目录名以 '/' 结尾），rel 为空表示根目录
bool codeListDir(const std::string &workspace, const std::string &rel,
                 std::vector<std::string> &out, std::string &error);

// ------------------------------------------------------------------
// TypeScript 运行时说明（写进 system prompt 的一段话）
//
//   本框架的 JS 运行时是自己的 JZJSEngine(QuickJS), **不是 Node**: 能写什么模块、
//   怎么导入、进程什么时候退出都跟 Node 不一样。这些边界模型猜不出来, 只能靠提示词
//   告诉它 —— 否则它会按 Node 习惯写 require / 具名导入 / 只 listen 不留心跳, 一路撞
//   到"编译过了但跑不起来"。三个宿主(code_agent / agent_cli / 评测)共用这一份,
//   避免各写一段然后漂移。
// ------------------------------------------------------------------
std::string jzTsRuntimeNotes();

// 同上，另一段：终端怎么跑"起服务 + 测"这类长任务（terminal_run 的 background / query / cancel）。
std::string jzTerminalRunNotes();

enum class ToolOperator
{
    Read,
    Write,
    Execute,
};

// ------------------------------------------------------------------
// 工具拒绝原因（结构化）
//   只用来让上层(宿主)区分"真的失败了"与"被策略拦了"，
//   从而对"反复被拦"这类卡死行为做统计与熔断。
//   普通的调用错误(参数缺失、文件过大等)用 None，由"重复调用"那道闸兜住。
// ------------------------------------------------------------------
enum class DenyReason
{
    None = 0,           // 非拒绝类失败
    PathOutside,        // 路径越界，不在工作区/授信区内
    AccessDenied,       // 区域策略不允许该操作(如区外只读 → 写被拒)
    NotFound,           // 文件/目录/程序不存在
    WriteDenied,        // 不允许写   
    CommandDenied,      // 命令被策略禁止
    ProgramDenied,      // 程序不在允许执行的区域
    EnvChange,          // 会修改环境 / 下载安装
    EditFailed          // 编辑类失败(old_str 不匹配、匹配多处等)
};

// 拒绝原因的中文说明（用于日志与提示）
const char *denyReasonText(DenyReason reason);

// 工具宿主：工具唯一需要外部提供的东西。定义见本文件末尾（它要用到
// JZVirtualTerminalConfig 这个类型）。
class ICodeToolHost;

// ------------------------------------------------------------------
// 所有代码工具的基类：
//   - 统一管理工作区根目录与路径越界校验
//   - 统一的结果回传（finish）
//
// 本类**不认识** JZCodeAgent：宿主一侧的能力全部经 ICodeToolHost 拿，
// 所以同一份实现既能给本地 code_agent 用，也能给远端 agent_node 用。
// ------------------------------------------------------------------
class JZCodeTool : public JZAgentTool
{
public:
    JZCodeTool();
    virtual ~JZCodeTool();

    // 绑定宿主（可传 nullptr：测试/无宿主环境下路径检查直接放行）
    void setHost(ICodeToolHost *host);
    ICodeToolHost *host() const { return m_host; }

    DenyReason checkPathPermission(ToolOperator op, std::string path);

    static ParamSchema makeParam(const std::string &name, ParamType type, const std::string &desc);

protected:
    void finish(const ToolCall &call, bool ok, const std::string &text,
                DenyReason deny = DenyReason::None);

    std::string fullPath(std::string path);

    ICodeToolHost *m_host = nullptr;
};

// ------------------------------------------------------------------
// read_file: 读取文本文件内容（带行号）
//   用 start_line / line_count 只读某段行范围，避免把大文件整篇灌进上下文；
//   单次最多返回 2000 行，超出请分段。带行号返回便于后续定位；
//   二进制文件直接拒绝。读文件优先用本工具而不是 `cat`
//   —— 行号 / 分段 / 编码判定 shell 都给不了。
// ------------------------------------------------------------------
class JZReadFileTool : public JZCodeTool
{
public:
    JZReadFileTool();
    virtual void call(const ToolCall &params) override;
};

// ------------------------------------------------------------------
// write_file: 创建或整文件覆盖写入
//   用 content 整体覆盖一个文件（新建 / 完全重写）。
//   局部片段修改请用 modify_file —— 它校验唯一匹配且回上下文，sed 给不了这些。
// ------------------------------------------------------------------
class JZWriteFileTool : public JZCodeTool
{
public:
    JZWriteFileTool();
    virtual void call(const ToolCall &params) override;
};

// ------------------------------------------------------------------
// modify_file: 局部精确替换（不改其它内容）
//   old_str 必须非空且唯一匹配（replace_all=true 则全部替换），用 new_str 替换它。
//   整文件写入请用 write_file；本工具只做"片段级"修改。
//
// 为什么这一条留成**工具**而不是让模型用 shell:
//   `sed` 不校验唯一匹配 —— 改代码时很容易改错地方; 而且本工具成功时回上下文片段
//   (让模型确认改对了地方)、失败时回命中位置(帮它补上下文), 这两点 shell 都给不了。
// 其它文件操作(ls/cat/mkdir/rm/cp/mv)都有等价的 shell 命令, 所以不再单独做工具。
// ------------------------------------------------------------------
class JZModifyFileTool : public JZCodeTool
{
public:
    JZModifyFileTool();
    virtual void call(const ToolCall &params) override;
};

// ------------------------------------------------------------------
// get_datetime: 获取当前日期时间
//   模型在需要"今天 / 现在 / 星期几"这类信息时，应优先调用本工具，而不是依赖
//   训练数据里的时间。默认返回结构化 JSON（date/time/weekday/timestamp/datetime），
//   也可传 format 用 strftime 风格字符串自定义输出；utc=true 时返回 UTC 而非本地时间。
// ------------------------------------------------------------------
class JZGetDateTimeTool : public JZCodeTool
{
public:
    JZGetDateTimeTool();
    virtual void call(const ToolCall &params) override;
};


// ------------------------------------------------------------------
// search_context: 在工作区内搜索**文本 / 正则**(找"这个串在哪出现过")
//   path 可以是目录(递归搜索)也可以是单个文件；regex=true 时 pattern 按正则解析，
//   正则编译失败直接报错(不会静默变成"没有匹配")。
//   返回的路径统一相对工作区 —— 可以原样喂回 read_file / modify_file。
//   跳过隐藏目录与 build/out 等生成目录；二进制 / 超过 2MB / 读取失败的文件会被跳过，
//   并在结果末尾注明跳过了多少个，避免模型把"被跳过"当成"没有匹配"。
//   ⚠ 找**文件名**用 search_file(不要再用 file_filter 硬凑)。
// ------------------------------------------------------------------
class JZSearchContextTool : public JZCodeTool
{
public:
    JZSearchContextTool();
    virtual void call(const ToolCall& params) override;
};

// ------------------------------------------------------------------
// search_file: 按**文件名 / 相对路径**找文件(找"那个文件在哪")
//   pattern 是通配符(* ?)，可逗号分隔多个；命中规则 = 文件名或"相对工作区路径"任一匹配
//   (所以既能写 "*.d.ts"，也能写 "js_wrapper/*/types/*.d.ts")。
//   只回路径(目录带尾 "/")、不回内容 —— 找内容用 search_context，看内容用 read_file。
//   与 search_context 同一套跳过规则(隐藏目录、build/out/bin 等生成目录)与同一套扫描预算；
//   结果末尾会注明"跳过了哪些目录""是否触顶"，避免把"没返回"当成"不存在"
//   (真要找生成目录里的东西，直接把 path 指到那个目录 —— 跳过只作用于递归的子目录)。
// ------------------------------------------------------------------
class JZSearchFileTool : public JZCodeTool
{
public:
    JZSearchFileTool();
    virtual void call(const ToolCall& params) override;
};

// ------------------------------------------------------------------
// memory: 跨任务的长期记忆(一个纯文本文件 + read / append / replace 三个动作)
//   记什么: 跨任务还能用上的结论 —— 环境事实(构建/运行命令、程序路径)、踩过的坑、
//           接口/约定、最终结论。不是流水账(流水账在对话历史里, 每轮重放只会烧 token)。
//
//   为什么做成工具而不是"让模型自己用 write_file/modify_file 维护一个 md":
//     - append 语义: 模型不用 read-modify-write 整个文件(长记忆时这一步极易写坏/截断)
//     - 时间戳 + 单条一行, 格式统一, 注入时好读
//     - 有大小上限, 不会无限膨胀(超了让模型先 replace 整理)
//     - 文件工具被关掉(allowFileTools=false)时记忆仍然可用
//
//   路径由宿主 setup 给定 —— 模型改不了(工具没有 path 参数 = 没有越界面), 所以这里
//   不做路径审核; 但要知道这条写权限是宿主授权的: 配到工作区外的路径就等于往外写。
//   读取/注入的上限是同一个数: 超了只给**最近的**(append 语义下新的在后面), 旧内容还在
//   文件里, 模型可以用 read_file 分段看, 或者用 replace 精简。
// ------------------------------------------------------------------
class JZMemoryTool : public JZCodeTool
{
public:
    JZMemoryTool();

    // file: 记忆文件全路径(为空则不可用); maxBytes: 注入/读取上限(<=0 取 8192)
    void setup(const std::string &file, int maxBytes);

    virtual void call(const ToolCall &params) override;

    bool enabled() const { return !m_file.empty(); }
    const std::string &file() const { return m_file; }

    // 读记忆(超上限只返回尾部)。返回空串 = 还没有记忆(文件不存在/为空)。
    // 宿主"开工注入"和工具的 read 动作走同一份逻辑 —— 模型读到的 == 注入的, 不两套。
    // totalBytes/truncated 可选回传原始大小与是否被截断。
    std::string readMemory(int *totalBytes = nullptr, bool *truncated = nullptr) const;

private:
    bool readRaw(std::string &text) const;                        // 整份原文(不截断, 去 BOM)
    bool writeAll(const std::string &text, std::string &error) const;

    std::string m_file;
    int m_maxBytes = 8192;
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
    virtual void cancel(const std::string &id) override;

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
class JZShellTool : public JZProcessRunner
{
public:
    JZShellTool();

    void setTimeOut(int timeout);

    // 额外的 exe 搜索路径：会在执行命令前注入到 shell 的 PATH 最前面，
    // 使得工作区内编译产物(如 build/Debug/demo.exe)或额外 SDK 目录下的程序
    // 可被直接按名调用，而不必写全路径。
    void setExeSearchPaths(const std::vector<std::string> &paths);

    virtual void call(const ToolCall &params) override;

private:
    int m_timeoutMs = 120000;
    std::vector<std::string> m_exeSearchPaths;   // 额外 exe 搜索目录
};

// ------------------------------------------------------------------
// 固定命令工具: 执行 setup 时给定的命令（模型不能改命令本身）
//   args 为 true 时，工具带一个可选 args 参数，其内容会追加到命令之后
//   （"程序 + 参数"形态）；args 为空则原样执行命令。
//   args 为 false 的工具不接受任何追加，行为与原版完全一致。
//   ⚠ 2026-10-14: code_agent 侧已不再注册用它包出来的 run_js 工具（编译/运行统一收敛到终端里
//     的 `tsc -p tsconfig.json` / `code_runtime`）；类本身保留给其它宿主按需使用。
// ------------------------------------------------------------------
class JZCommandTool : public JZProcessRunner
{
public:
    JZCommandTool();

    // args 为 true 时：为工具定义可选 args 参数，调用时追加到 command 后
    void setup(const std::string& name, const std::string& desc,
        const std::string& command, bool args, const std::string& defalut_work_dir = std::string());

    virtual void call(const ToolCall& params) override;

private:
    std::string m_command;
    std::string m_defaultWorkDir;
    bool m_hasArgs = false;
    int m_timeoutMs = 120000;
};

// ------------------------------------------------------------------
// build_project: 打包/发布当前工作区的 TS 工程
//   走宿主可选能力 ICodeToolHost::toolBuildProject（本地实现=JZCodeAgent::buildProject，
//   异步 tsc 全量编译），编译结束后自动校验入口产物 main.js 并生成/刷新 project.json；
//   编译错误回传给模型自修。打包完成后在产物目录跑 code_runtime 即可运行。
//   不提供 toolBuildFinished() 的宿主(远端节点)用不了本工具。
//   ⚠ 2026-10-14: code_agent 侧**不再把它注册给模型**（编译由模型自己在终端跑 `tsc -p tsconfig.json`）；
//     宿主侧 API buildProject() 仍在用（评测宿主打包判定 / 面板"编译"按钮 / exportProject）。
// ------------------------------------------------------------------
class JZBuildProjectTool : public JZCodeTool
{
public:
    JZBuildProjectTool();

    virtual void call(const ToolCall &params) override;
    virtual void cancel(const std::string &id) override;

private:
    void onBuildDone(bool ok, const std::string &error);

    ToolCall m_pending;      // 等待打包结束回填的调用
    bool m_connected = false;
};

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
    Shell,              // 基础 shell / coreutils（ls cat grep mkdir rm cp mv ...）：默认允许。
                        //   单独一档是因为"命令都走 shell"之后，不认这些等于什么都跑不了 ——
                        //   它们就是文件工具被删掉以后的替代品。名单两边通用：
                        //   Linux 系统自带；Windows 由 Git Bash 提供（同名）。
    PackageManager,     // pip/npm/conda 等：默认拒绝，设置里可开
    Network,            // curl/wget 等：默认拒绝，设置里可开
    SystemAdmin,        // reg/sc/net/setx 等：永不放行
    Dangerous           // dd/mkfs/sudo/shutdown/chown 等破坏性或系统级命令：永不放行
                        //   （动磁盘、改系统、提权、杀别人的进程 —— 手滑一次就是重装系统）
};

struct ProgramSpec
{
    std::string name;
    std::vector<std::string> exeNames;      // 可执行文件全名（同时兼容有无 .exe）
    CmdCategory category = CmdCategory::Tool;
    std::vector<std::string> denyFlags;     // 禁止出现的参数（前缀匹配）
    std::vector<std::string> denyModules;   // 配合 -m：挡掉 python -m pip 这类
};

// 显式指定的可信程序：用全路径直接定位，优先于在授信目录里按名搜索。
// name 需与白名单 ProgramSpec 的 name 对应（如 cmake / python）。
struct TrustedProgram
{
    std::string name;   // 程序名（小写，与 ProgramSpec 的 name 对应）
    std::string path;   // 可执行文件全路径
};

// ------------------------------------------------------------------
// 工具宿主接口
//
// 工具向外唯一需要的东西: 三件必需的 + 一个可选能力(打包)。
//   - 本地 code_agent: JZCodeAgent 实现它(工作区/授信目录走 CodeAgentConfig,
//     读文件走它自己的 LRU 缓存, 打包转调 buildProject)
//   - 远端 agent_node: 自己实现一份(node 侧没有 TS 工程, 打包用默认实现)
// 这样工具实现与权限规则全仓只有一份, 本地和远端行为逐字一致。
// ------------------------------------------------------------------
class ICodeToolHost
{
public:
    virtual ~ICodeToolHost() {}

    // 终端配置: 目录边界(zone) + 权限分档 + 命令策略, 见 JZVirtualTerminalConfig.h。
    // 注意: zone.workspace 为空时前缀比较会放行一切 ——
    // 实现方必须保证它非空（节点侧默认取进程当前工作目录）。
    virtual const JZVirtualTerminalConfig &toolTerminalConfig() const = 0;

    // 读文件（absPath 必须是绝对路径）。失败返回 false 并把原因写进 error。
    // 宿主可以在这里挂缓存；不想要缓存就直接读。
    virtual bool toolReadFile(const std::string &absPath, std::string &text, std::string &error) = 0;

    // ---- 可选能力：打包/发布（只有本地 TS 工程宿主支持）----
    virtual bool toolIsBuildRunning() const { return false; }
    virtual bool toolBuildProject(std::string &error)
    {
        error = "当前宿主不支持打包/发布";
        return false;
    }
    // 打包结束信号；返回 nullptr 表示永远不会有结果（工具会直接失败）
    virtual JZSignal<bool, const std::string &> *toolBuildFinished() { return nullptr; }
};

// ------------------------------------------------------------------
// web_search: 联网搜索
//   用 JZNetworkAccessManager 发起 HTTP GET 到搜索接口，解析结果（标题/链接/摘要）返回模型。
//   支持多源自动回退: 候选源按序尝试，单个源出现网络错误 / 非 2xx / 返回内容无法识别时，
//   自动切换下一个候选源，全部失败才报错。
//   未配置模板时内置默认源（无需 API Key）: Bing RSS（format=rss，常规网页结果）。
//   响应 JSON / RSS 均可解析；如需多源自动回退，在 call() 里追加候选 URL 即可。
//   自定义模板: setSearchUrlTemplate 配置，模板含 {q} 占位符（URL 编码后的查询词），
//   配置后只使用该自定义源（也应返回 JSON 或 RSS）。
//   网络访问管理器由调用方通过 setNetworkAccessManager 注入（所有网络工具共享同一个，
//   代理由调用方在该管理器上统一设置一次，http/https 均生效），工具本身不持有所有权。
// ------------------------------------------------------------------
class JZWebSearchTool : public JZCodeTool
{
public:
    JZWebSearchTool();

    void setSearchUrlTemplate(const std::string &urlTemplate);
    // 注入共享的网络访问管理器（非拥有）；代理由调用方在管理器上统一设置
    void setNetworkAccessManager(JZNetworkAccessManager *net);

    virtual void call(const ToolCall &params) override;
    // 取消某次调用：只关掉**它自己**在途的请求，不碰别的调用
    virtual void cancel(const std::string &id) override;

private:
    // ⭐ 每次调用一份上下文 —— 本工具必须支持被并行调用。
    //   "多源回退"天生是异步多步链（一个查询可能连试几个源）。以前把状态放在成员变量里，
    //   并且"新调用会 close 掉上一次在途的请求"：模型一轮里同时发两个 web_search 时互相踩，
    //   旧调用的回调因"reply 已被替换"直接 return ⇒ 那个调用的结果**永远不回**，
    //   而 agent 会一直等它那一格(m_pendingTools) ⇒ 整轮静默停住（2026-10-15 实测）。
    //   现在每调用一份 ctx，各自持有自己的候选队列与在途 reply，互不干扰。
    struct SearchCall
    {
        int ctxId = 0;                     // 本工具内的自增号（不依赖 call.id —— 它可能为空）
        ToolCall call;                     // 原样带着：回包必须用同一个 id
        std::string query;                 // 本轮查询词（回退链多次请求共用）
        int num = 5;                       // 本轮结果条数上限
        std::vector<std::string> queue;    // 待尝试的候选源 URL（{q} 尚未展开）
        std::string currentUrl;            // 当前在请求的源（错误定位用）
        std::string errors;                // 各候选源失败原因累积
        JZNetworkReply *reply = nullptr;   // 本调用在途的请求（由本 ctx 独占）
    };

    void runNextSearch(const std::shared_ptr<SearchCall> &ctx);       // 从候选队列取下一个源并请求
    void failOverSearch(const std::shared_ptr<SearchCall> &ctx, const std::string &reason); // 切下一个候选源
    void handleReply(const std::shared_ptr<SearchCall> &ctx, JZNetworkReply *reply);
    void parseSearch(const std::shared_ptr<SearchCall> &ctx, const std::string &body);
    void finishSearch(const std::shared_ptr<SearchCall> &ctx, bool ok, const std::string &text);

    JZNetworkAccessManager *m_net = nullptr;   // 非拥有，来自外部注入
    std::string m_searchUrlTemplate;   // 搜索接口模板；构造函数默认内置 kBingRssSearchUrl，可 setSearchUrlTemplate 覆盖

    std::map<int, std::shared_ptr<SearchCall>> m_calls;   // 在途调用：ctxId -> ctx
    int m_ctxIdGen = 0;                                   // ctxId 自增（并行调用靠它区分）
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
    // 取消某次调用：只关掉**它自己**在途的请求，不碰别的调用
    virtual void cancel(const std::string &id) override;

private:
    // 与 web_search 同理：每次调用一份上下文，支持同一工具被并行调用
    // (以前共享 m_reply 且"新调用取消上一次" ⇒ 一眼看去像串行工具, 并发就会互相踩)
    struct FetchCall
    {
        int ctxId = 0;
        ToolCall call;
        int maxChars = 20000;
        JZNetworkReply *reply = nullptr;
    };

    void handleReply(const std::shared_ptr<FetchCall> &ctx, JZNetworkReply *reply);
    void finishFetch(const std::shared_ptr<FetchCall> &ctx, bool ok, const std::string &text);

    JZNetworkAccessManager *m_net = nullptr;   // 非拥有，来自外部注入
    std::map<int, std::shared_ptr<FetchCall>> m_calls;
    int m_ctxIdGen = 0;
};

#endif // !JZ_CODE_TOOLS_H_
