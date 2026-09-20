#ifndef JZ_VIRTUAL_TERMINAL_H_
#define JZ_VIRTUAL_TERMINAL_H_

#include <functional>
#include <map>
#include <string>
#include <set>
#include <vector>

#include "JZCodeTools.h"   // JZVirtualTerminalConfig / TrustedProgram / DenyReason
#include "JZObject.h"
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
        int64_t lastActive = 0;  // 毫秒时间戳
    };

    // 命令结果回调: ok=false 时 error 有原因(output 可能是已拿到的部分)
    typedef std::function<void(bool ok, const std::string &output, const std::string &error)> RunCallback;

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
        std::vector<RunLogLine> log;
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

    // 跑命令用的 shell。自动探测: POSIX = /bin/sh。
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
    // 扩展参数(均有默认值):
    //   runId    : 非 nullptr 时回填本次运行的 id(供 terminal_query 引用)
    //   onChunk  : 非空的逐块输出回调(流式)
    //   background: true 时命令在后台跑, run() 返回即启动, 不阻塞 cb
    bool run(const std::string &command, int timeoutMs, const RunCallback &cb, std::string &error,
             std::string *runId = nullptr,
             const std::function<void(const std::string &stream, const std::string &chunk)> &onChunk = nullptr,
             bool background = false);
    bool cancel();                       // 停掉本终端在跑的命令
    std::string takeOutput();            // 取一次累积输出并清空

    // ==================== 输出过滤 ====================
    // 开关开(默认开)时所有命令的输出都做截断: 编译命令(build set 见 cpp isBuildCommand)
    // 走"错误上下文"特殊处理(成功尾部 20 行 / 失败开头约 100 行错误且错误块完整);
    // 其它命令超长时保留头 50 + 尾 50 行、中间省略。完整日志仍可由 terminal_query 取。
    // 模型可在 terminal_run 用 error_context=false 关掉。
    void setFilterOutput(bool on) { m_filterOutput = on; }
    bool filterOutput() const { return m_filterOutput; }

    // ==================== 运行记录(后台/流式留下的日志) ====================
    bool runSnapshot(const std::string &runId, RunRecordInfo &out) const;
    std::vector<std::string> runIds() const;   // 本终端的全部运行 id(按发生先后)

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
        std::vector<RunLogLine> log;
    };

    void touch();                       // 刷新活跃时间 + 记审计
    // 把一块输出追加到本终端(合并)与运行记录(逐行, 带流标签), 并按需推给流式回调
    void appendRunOutput(const std::string &stream, const std::string &chunk);

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
    bool m_running = false;             // 有命令在途
    int64_t m_lastActive = 0;

    // ---- 在途命令 ----
    // proc 随终端建一次(不是每条命令新起一个), 命令跑完后同一个对象可以再 start。
    // 所有权在终端(this), 析构时 killProcess + delete。
    JZProcess *m_proc = nullptr;
    JZTimer *m_timeoutTimer = nullptr;
    std::string m_output;               // 累积输出(合并 stdout/stderr), takeOutput 取一次清一次
    bool m_outputTruncated = false;     // m_output 超过字节上限被裁过, takeOutput 时补一次标记
    bool m_timedOut = false;
    RunCallback m_cb;                   // 在途命令的应答回调
    std::string m_runId;                // 当前运行的记录 id(供 appendRunOutput 反查)
    std::function<void(const std::string &, const std::string &)> m_onChunk;  // 流式逐块回调

    std::string m_shell;                // 见 shellProgram(): 构造时自动探测
    int m_recycleMs = 15 * 60 * 1000;
    int m_runIdGen = 0;
    bool m_filterOutput = true;         // 编译输出错误截取(默认开, 见 setFilterOutput)

    // 运行记录(后台/流式): runId -> 记录, 发生顺序另用 m_runOrder 保序
    std::map<std::string, RunRecord> m_runs;
    std::vector<std::string> m_runOrder;

    AuditCallback m_audit;
};

#endif // !JZ_VIRTUAL_TERMINAL_H_
