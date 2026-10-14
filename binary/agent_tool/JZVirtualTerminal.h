#ifndef JZ_VIRTUAL_TERMINAL_H_
#define JZ_VIRTUAL_TERMINAL_H_

#include <deque>
#include <functional>
#include <map>
#include <string>
#include <set>
#include <vector>

#include "JZCodeTools.h"   // JZVirtualTerminalConfig / TrustedProgram / DenyReason
#include "JZObject.h"
#include "JZResult.h"       // run() 返回 JZResult<std::string>(成功=run_id)
#include "JZShellParser.h"  // JZShell::Command(安全底线要用)

class JZProcess;
class JZTimer;

// ---------------------------------------------------------------------------
// JZVirtualTerminal —— 单台终端的执行面与审核面
//
// 设计(2026-09-17 调整): 一个 JZVirtualTerminal 就是"一个终端",由上层
// (如 JZAgentNode 用 map<session, JZVirtualTerminal> 管理)负责创建/销毁与
// 按会话路由。本类**不再维护多会话**: cwd / 环境变量 / 在跑进程 / 运行记录
// 都是"这一个终端"自己的状态。
//
// 为什么收成一个类: 任何"会碰这台机器"的动作都从这里走 —— 先审核, 再执行。
// 三块职责: 审核(checkRead/checkWrite/checkExec 唯一入口) / 内建动作
// (列目录/读/写/编辑/建目录/删/拷/移/cd) / 裸命令口子 run()(交给 shell)。
//
// ⛔ 参数一律结构化, 绝不拼命令行字符串。
// ⚠ 命令由我们解释(不挂常驻 shell): 每条命令一个 JZProcess, cwd/env 由我们维护。
//   好处: 状态可序列化、可审计, `a && b` 能逐条审权限。代价: 交互式程序做不了。
//
// 并发(2026-09-21): 一个终端可以**同时跑多条命令** —— 每次 run() = 一条在途运行, 各带
//   自己的 JZProcess + 超时定时器 + 输出 + 回调, 按 run_id 区分(做法同 JZProcessRunner
//   的多任务列表)。以前是"终端只有一个 proc / 一条在途命令", 第二条只能等第一条结束
//   (还会直接报错)。cwd 仍是终端级的: 每条命令按**启动那一刻**的 cwd 跑。
// 本类不是 JZAgentTool —— 是否暴露、以什么名字暴露由上层决定
// (本地 code_agent 直接持有; 远端 agent_node 经 JZTerminalRegistry 包成 MCP 工具)。
//
// ⚠ 平台定位: 只跑 Linux。
// ⚠ "都走 shell": 管道/重定向/串联不自己解析, 整条命令交给 /bin/sh -c。
//   因此审核是命令级的(每段首 token 过白名单 + 重定向目标过写权限)。
// ---------------------------------------------------------------------------
class JZVirtualTerminal : public JZObject
{
public:
    // ---- 审核结论 ----
    enum class Decision
    {
        Allow,
        Deny,
        Ask,           // 自动策略判不了, 需要人拍板(接 MCP input_required)
    };

    struct Verdict
    {
        Decision decision = Decision::Deny;
        DenyReason reason = DenyReason::None;
        std::string detail;                      // 给人 / 给模型看的原因

        bool allowed() const { return decision == Decision::Allow; }
        bool needAsk() const { return decision == Decision::Ask; }
    };

    // ---- 终端快照(给列表/状态用, 不暴露内部对象) ----
    // 注意状态是我们自己维护的(不是某个常驻 shell 的): cwd + 环境变量覆盖。
    struct SessionInfo
    {
        std::string id;        // 上层分配的会话 key(由 setId 设置, 终端逻辑本身不用)
        std::string cwd;
        bool running = false;    // 有命令在途
        int runningCount = 0;    // 在途运行的条数(一个终端可以同时跑多条)
        int64_t lastActive = 0;  // 毫秒时间戳
    };

    // 命令结果回调: ok=false 时 error 有原因(output 可能是已拿到的部分)
    typedef std::function<void(bool ok, const std::string &output, const std::string &error)> RunCallback;

    // 单次运行的输出过滤开关(见 setFilterOutput)。
    // 为什么按次给而不是只留终端级开关: 一个终端能同时跑多条命令, 终端级开关会互相串 ——
    // 后启动的那条会把前面那条的过滤行为改掉。
    enum class OutputFilter
    {
        Default,   // 用终端设置(见 setFilterOutput)
        On,
        Off,
    };

    // 后台/流式运行留下的逐行日志(供 terminal_query 拉取, 支持最近 N 行、按流/关键字过滤)
    struct RunLogLine
    {
        std::string stream;   // "stdout" / "stderr"
        std::string text;     // 一行输出(已去掉行尾 \r\n)
    };

    // 一次运行的快照(给 terminal_query 用)
    struct RunRecordInfo
    {
        std::string runId;
        std::string command;
        std::string status;   // "running" / "done"
        int exitCode = -1;
        bool timedOut = false;
        std::string error;    // 收尾原因(超时/被杀/非零退出), 可为空
        bool background = false;   // 后台运行起的(见 runBackground): 没有超时, 启动即返回
        int pid = -1;              // 根进程号(本终端起的; 展示与 kill 归属判定用)
        int64_t startTime = 0;     // 启动时刻(毫秒), 查"已经跑了多久"用
        int64_t endTime = 0;       // 结束时刻(毫秒), 还在跑时为 0(算总耗时用)
        std::vector<RunLogLine> log;

        // ---- 日志的"绝对行号"信息(给 terminal_query 做增量读取) ----
        // 记录有上限, 旧的会从头部丢 ⇒ log[0] 的绝对行号不再是 0。这里给出:
        //   totalLines: 这条运行**累计产出**过多少行(只增不减)
        //   firstLine : log[0] 的绝对行号(= totalLines - log.size(), 更早的已被丢弃)
        //   readPos   : 上次 terminal_query 读到哪(增量查询从这里接着读)
        // 绝对行号 = 从这条运行的第一行开始数, 与 readPos 同一个坐标系。
        size_t totalLines = 0;
        size_t firstLine = 0;
        size_t readPos = 0;
    };

    explicit JZVirtualTerminal(JZObject *parent = nullptr);
    ~JZVirtualTerminal() override;

    // 上层分配的会话 key(仅用于 list/query 展示, 不参与终端逻辑)
    void setId(const std::string &id) { m_id = id; }
    std::string id() const { return m_id; }

    // ==================== 策略(审核依据) ====================
    // 终端配置: 目录边界 + 权限分档 + 命令策略(见 JZVirtualTerminalConfig.h)。
    // 换工作区时会把 cwd 拉回工作区根(旧 cwd 可能已经失效)
    void setTerminalConfig(const JZVirtualTerminalConfig &config);
    const JZVirtualTerminalConfig &terminalConfig() const;

    // 显式指定的可信程序(全路径): 优先于在授信目录里按名搜索
    void setTrustedPrograms(const std::vector<TrustedProgram> &programs);
    const std::map<std::string, std::string> &trustedPrograms() const;

    // 查配置里的程序映射(JZVirtualTerminalConfig::programPaths):
    // 返回该名字对应的可执行文件全路径, 没有则返回空串。
    std::string programPath(const std::string &name) const;

    // 程序白名单。构造时会装一份默认表。
    void addProgram(const std::string &name, std::vector<std::string> exeNames, CmdCategory category,
                    std::vector<std::string> denyFlags = std::vector<std::string>(),
                    std::vector<std::string> denyModules = std::vector<std::string>());
    std::vector<std::string> availablePrograms() const;   // 供提示词/自检

    // 跑命令用的 shell(整条命令交给它 `-c` 执行; 也是"没有 shebang 的脚本"的兜底解释器)。
    // 自动探测: **优先 bash**(Windows 找 Git Bash 的安装目录, POSIX 先看 /bin、/usr/bin 再扫
    // PATH), 找不到回退 sh(/bin/sh 或 PATH 里的 sh)。两个都没有才为空 ⇒ run() 明确报错,
    // 不静默退回别的解释器。
    // 探测结果会进模型提示词(见 JZAgentCli::buildSystemPrompt 的 Shell notes), 模型据此
    // 决定写不写 bash 专有语法。要固定: setShellProgram() 或配置 terminal.shellProgram。
    void setShellProgram(const std::string &program);
    std::string shellProgram() const { return m_shell; }

    // ==================== 审核: 唯一入口 ====================
    Verdict checkRead(const std::string &path) const;
    Verdict checkWrite(const std::string &path) const;
    Verdict checkExec(const std::string &program) const;
    // base: 审核时解析相对路径用的基准目录, 空 = 用本终端的 cwd(工作区根)。
    // `cd X && cmd` 这种, sh 会真的 chdir 过去, 静态检查必须跟着走进去,
    // 否则审的是工作区里的路径、实际写的却是 X 里的 —— 审核就成了摆设。
    Verdict checkExec(const std::string &program, const std::vector<std::string> &args,
                      const std::string &base = std::string()) const;

    // 解析一个路径参数: 相对路径按 base(默认本终端 cwd) 拼, 然后按 writeAccess 审核。
    bool resolvePath(const std::string &path, bool writeAccess,
                     std::string &absPath, Verdict &verdict, std::string &error,
                     const std::string &base = std::string()) const;

    // ==================== 终端状态 ====================
    // 本终端的快照(id/cwd/running/lastActive), 给上层 list 用。
    SessionInfo info() const;

    // 空闲多久回收(毫秒), 默认 15 分钟; <=0 表示不回收。语义交给上层(生命周期由上层管)。
    void setRecycleMs(int ms) { m_recycleMs = ms; }
    int recycleMs() const { return m_recycleMs; }

    // ==================== 动作(内建, 不经 shell; 内部先审核) ====================
    bool listDir(const std::string &path, std::vector<std::string> &names, std::string &error);
    bool readFile(const std::string &path, std::string &text, std::string &error);
    bool writeFile(const std::string &path, const std::string &text, bool append, std::string &error);
    // oldStr 为空 = 整文件覆盖; 否则精确替换, replaceAll=false 时要求唯一匹配
    bool editFile(const std::string &path, const std::string &oldStr, const std::string &newStr,
                  bool replaceAll, std::string &error);
    bool makeDir(const std::string &path, std::string &error);
    bool removePath(const std::string &path, bool recursive, std::string &error);
    bool copyPath(const std::string &from, const std::string &to, std::string &error);
    bool movePath(const std::string &from, const std::string &to, std::string &error);
    bool changeDir(const std::string &path, std::string &error);
    bool printDir(std::string &cwd) const;

    // ==================== 命令 ====================
    // 语法由我们解析(不是 shell): 支持 && || ; 串联、| 管道、> >> < 重定向、通配符与变量展开。
    // ⭐ 可以并发: 同一个终端同时跑多条命令, 每条各自一个进程/超时/输出/回调, 靠 runId 区分。
    // run() 自己**不阻塞**(起完进程就返回, 结果走 cb)。
    // 返回: 成功 = 本次运行的 run_id(供 terminal_query / cancel 引用); 失败 = 原因
    // (命令被拒 / 起不来 —— 这时 cb 也会被同步调一次 ok=false)。
    // 扩展参数(均有默认值):
    //   onChunk  : 非空的逐块输出回调(流式)
    //   background: 兼容旧签名(启动即返回, 多路并发后已无实际区别)
    //   filter   : 本次运行的输出过滤开关(见 OutputFilter)
    //   前台超时: 硬上限 15 分钟(见实现: timeoutMs>15min 或<=0 时归一到 15min 并按时强杀);
    //             后台(background)不受此限(本就不设定时器)。
    JZResult<std::string> run(const std::string &command, int timeoutMs, const RunCallback &cb,
                              const std::function<void(const std::string &stream, const std::string &chunk)> &onChunk = nullptr,
                              bool background = false,
                              OutputFilter filter = OutputFilter::Default);

    // 后台运行(2026-09-22): 起一条命令后**立即返回**, 不等它结束, 也不设超时 ——
    //   给"自己不退出"的长程任务用(起服务 / 常驻进程 / 长时间构建), 这类命令前台跑必然
    //   撞死在 timeout 上(超时还会把进程杀掉, 那正是以前"起服务"用例最大的坑)。
    // 命令本身还是**普通命令**: 同样过审核、同样由本终端起进程、同样收输出 —— 区别只在
    //   "我们不等它 / 不定时杀它", 进程始终在管理之下(不像 shell 的 `&` 那样脱离)。
    // 返回: 成功 = run_id(之后用 runSnapshot(runId) 查状态与日志、cancel(runId) 停掉);
    //       失败 = 命令被拒或起不来(这时不留记录, 也没有东西可查)。
    // cb 可以为空: 没人对结果感兴趣时(纯"起个服务")照样收尾 —— 结果只写进运行记录,
    //   之后按 run_id 查得到(状态/退出码/日志)。
    // ⚠ 生命期: 后台运行与前台共用并发上限(kMaxConcurrentRuns); 终端析构时会把它们一起杀掉。
    JZResult<std::string> runBackground(const std::string &command,
                                        const RunCallback &cb = RunCallback(),
                                        OutputFilter filter = OutputFilter::Default);

    // 停一条命令: **必须带 runId**(在途运行的 id 见 activeRunIds() 或 run() 的返回值)。
    // 被杀的命令照样走它自己的 RunCallback(ok=false), 收尾只有一条路径。
    bool cancel(const std::string &runId);
    // 停掉本终端所有在跑的命令, 返回停掉了几条(各自照常收尾)。
    int cancelAll();
    std::vector<std::string> activeRunIds() const;   // 在途运行的 id(按启动先后)
    std::string takeOutput();                        // 取所有在途运行的累积输出并清空
    std::string takeOutput(const std::string &runId);   // 取指定运行的累积输出并清空

    // ==================== 输出过滤 ====================
    // ⚠ **默认关**(2026-10-14 按用户要求改成"原样返回, 不加工"): 命令回包直接给本次运行的原始
    //   合并输出, 不再从运行日志重渲染(那条路会给每行加 stdout:/stderr: 标签, 还按"编译/通用"
    //   规则头尾截断)。打开时才做截断: 编译命令(build set 见 cpp isBuildCommand)走"错误上下文"
    //   特殊处理(成功尾部 20 行 / 失败开头约 100 行错误且错误块完整); 其它命令超长保留头 50 +
    //   尾 50 行、中间省略。完整日志仍可由 terminal_query 取。
    // 模型可在 terminal_run 用 error_context=true 显式打开(默认 false = 原样全量)。
    // 这是**终端级默认值**: 单次运行可以用 run() 的 filter 参数覆盖(见 OutputFilter)。
    void setFilterOutput(bool on) { m_filterOutput = on; }
    bool filterOutput() const { return m_filterOutput; }

    // ==================== 运行记录(后台/流式留下的日志) ====================
    bool runSnapshot(const std::string &runId, RunRecordInfo &out) const;
    std::vector<std::string> runIds() const;   // 本终端的全部运行 id(按发生先后)

    // 把这条运行的"增量读取游标"前移(见 RunRecordInfo::readPos)。
    // terminal_query 默认只回上次之后的新增行, 读完就前移到这里 —— 轮询查进度时
    // 不会把同一段日志反复灌进模型的上下文(长任务上这是最大的浪费来源)。
    bool advanceRunReadPos(const std::string &runId, size_t pos);

    // ==================== 环境变量(本终端级覆盖) ====================
    bool setEnv(const std::string &name, const std::string &value, bool persistent);
    bool unsetEnv(const std::string &name);
    std::map<std::string, std::string> env() const;

    // ==================== 审计(预留) ====================
    typedef std::function<void(const std::string &action, const Verdict &verdict,
                               const std::string &detail)> AuditCallback;
    void setAuditCallback(const AuditCallback &cb) { m_audit = cb; }

protected:
    void buildDefaultPrograms();   // 装默认程序白名单(构造时调)

    // 应用配置里的程序映射(programPaths): 补白名单 + 记全路径。
    // 每次 setTerminalConfig() 都会重跑一遍 —— 映射是动态的, 换了配置就换一套程序。
    void applyProgramPaths();

    // 审核通过后, 把命令里的"短程序名"换成配置里的全路径:
    //   `tsc -p .` -> `D:/x/ts_compiler/tsc/tsc.exe -p .`
    // 走 AST: JZShell::rewritePrograms() 按 Command::programPos/programLen 定位,
    // 只动程序名那几个字符 —— 名字出现在参数里不会被误换。
    // ⚠ 必须在审核**之后**调: 审核认的是短名(白名单按名字查), 换成全路径走的就是
    //   路径审核(区外一律拒), 那是另一套判定。
    std::string expandProgramPaths(const JZShell::Line &line, const std::string &command) const;

    // `rm -rf /`、`dd of=/dev/sda` 这类: 单看程序名审不出来(rm/dd 本身在白名单里),
    // 得看参数组合。返回 nullptr 表示用法没问题。
    const char *dangerousArgReason(const std::string &program,
                                   const std::vector<std::string> &args,
                                   const std::string &base = std::string()) const;

    // 安全底线: 即便 AccessLevel::Unrestricted(全部不检查)也始终生效, 命令原样交给
    // shell 之前最后一道闸。覆盖两类永远不许的:
    //   1) 特别危险的指令(关机 / 格式化磁盘 / 往块设备写 ...)
    //   2) 删除工作区外的文件(rm / rmdir 的目标不在工作区树内 —— 含根目录本身)
    // 返回 Allow 表示通过底线(其余检查在不限制档里本来就跳过)。
    const char *criticalDangerReason(const std::string &program,
                                     const std::vector<std::string> &args) const;
    bool isDeleteCommand(const std::string &programLower) const;   // "rm"/"rmdir"...
    bool isWithinWorkspace(const std::string &absPath) const;      // 相对工作区根
    Verdict checkCriticalFloor(const JZShell::Command &cmd, const std::string &base) const;

    // kill 的归属判定: pid 是本终端启动的进程(含它的子进程)才算数。
    // 别人的进程 —— 包括 agent 自己 —— 一律不许 kill。
    bool isOwnProcess(int pid) const;

    // 一次运行的记录(后台/流式留下的日志)。stdout/stderr 分存, 可按流/关键字过滤。
    struct RunRecord
    {
        std::string runId;
        std::string command;
        std::string status = "running";   // "running" / "done"
        int exitCode = -1;
        bool timedOut = false;
        std::string error;
        bool background = false;          // 后台运行(见 runBackground): 没有超时
        int pid = -1;                     // 根进程号(起进程成功后回填)
        int64_t startTime = 0;            // 启动时刻(毫秒)
        int64_t endTime = 0;              // 结束时刻(毫秒; 0 = 还在跑)

        // 日志行: 用 deque —— 超上限时从头部丢, pop_front 是 O(1)。
        // (以前是 vector + erase(begin()), 每多一行就把整段搬一次: 高频输出的后台服务
        //  在 4000 行窗口上等于 O(n²), 白烧 CPU。)
        std::deque<RunLogLine> log;
        int64_t logBytes = 0;             // log 里文本+流标签的字节数(按字节淘汰用)
        size_t totalLines = 0;            // 累计产出的行数(只增; 见 RunRecordInfo::totalLines)
        size_t readPos = 0;               // terminal_query 的增量读取游标(绝对行号)

        // 每路输出"还没换行的那半截": 输出是按块读进来的, 一行的两半可能落在两次读里 ——
        // 只落**完整行**, 否则一行会被切碎成好几条日志(查询时看到的日志就错位了)。
        std::string partialStdout;
        std::string partialStderr;
    };

    // ---- 在途运行(可多个并发) ----
    // 一次 run() = 一个 RunTask = 一个 JZProcess + 一个超时 JZTimer, 各自一份输出与回调。
    // 只用 runId 认人(回调里按 runId 现查, 不存 RunTask* —— 列表会增减, 指针会失效)。
    struct RunTask
    {
        std::string runId;
        std::string command;                // 原文(报错/输出过滤用)
        JZProcess *proc = nullptr;
        JZTimer *timer = nullptr;           // 本次运行的超时定时器(timeoutMs<=0 则 null)
        bool timedOut = false;
        bool filterOutput = true;           // 本次运行是否做输出截断
        std::string output;                 // 本次运行的累积输出(合并 stdout/stderr)
        bool outputTruncated = false;       // output 超过字节上限被裁过(见 takeOutput)
        int64_t startTime = 0;
        RunCallback cb;
        std::function<void(const std::string &stream, const std::string &chunk)> onChunk;
    };

    // run() 的实现(审核 + 起进程): 失败时填 error 返回 false, 成功时回填 runId。
    // 拆出来是因为中间几十处 Deny 都是"填 error + return false", 包一层比逐处改成 JZResult 干净。
    bool startRun(const std::string &command, int timeoutMs, const RunCallback &cb, std::string &error,
                  std::string *runId,
                  const std::function<void(const std::string &stream, const std::string &chunk)> &onChunk,
                  bool background, OutputFilter filter);

    RunTask *taskOf(const std::string &runId);
    const RunTask *taskOf(const std::string &runId) const;
    void eraseTask(const std::string &runId);
    // 一次运行结束的收尾(唯一路径: 自然退出 / 超时被杀 / cancel 被杀)。
    // 记结果 -> 摘任务 -> 回调, 全部按 runId 走。
    void onRunFinished(const std::string &runId, JZProcess *proc, int exitCode,
                       JZProcess::ExitStatus status, const std::string &command);

    void touch();                       // 刷新活跃时间 + 记审计
    // 把一块输出追加到该次运行(合并)与运行记录(逐行, 带流标签), 并按需推给它的流式回调
    void appendRunOutput(const std::string &runId, const std::string &stream, const std::string &chunk);

    void audit(const std::string &action, const Verdict &verdict, const std::string &detail);

    // 把一个绝对路径按"读 or 写"审核, 通过则填 absPath
    bool checkPath(const std::string &path, bool writeAccess,
                   const std::string &action, std::string &absPath, std::string &error,
                   const std::string &base = std::string());

    JZVirtualTerminalConfig m_config;
    std::map<std::string, std::string> m_trustedPrograms;   // 小写程序名 -> 全路径
    std::map<std::string, ProgramSpec> m_programs;          // 小写程序名 -> 白名单项

    // 本终端启动过的进程根 pid(用于 kill 的归属判定)。
    // 不随进程退出清理: 超时被杀的只杀到 sh, 孙进程可能残留, 事后要能收拾掉。
    std::set<int> m_ownPids;

    // ---- 单终端状态(原 Session 的字段提到这里) ----
    std::string m_id;                   // 会话 key(上层设)
    std::string m_cwd;                  // 规范化后的绝对目录
    std::map<std::string, std::string> m_env;   // 终端级环境变量覆盖
    int64_t m_lastActive = 0;

    // ---- 在途运行(一次 run() 一条) ----
    // 每条各自一个 JZProcess + JZTimer, 所有权在终端(this), 结束/析构时 kill + 回收。
    std::vector<RunTask> m_tasks;

    std::string m_shell;                // 见 shellProgram(): 构造时自动探测
    int m_recycleMs = 15 * 60 * 1000;
    int m_runIdGen = 0;
    bool m_filterOutput = false;        // 输出截断/错误上下文截取(默认**关**=原样返回, 见 setFilterOutput)

    // 运行记录(后台/流式): runId -> 记录, 发生顺序另用 m_runOrder 保序
    std::map<std::string, RunRecord> m_runs;
    std::vector<std::string> m_runOrder;

    AuditCallback m_audit;
};

#endif // !JZ_VIRTUAL_TERMINAL_H_
