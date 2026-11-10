#ifndef JZ_VIRTUAL_TERMINAL_H_
#define JZ_VIRTUAL_TERMINAL_H_

#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <set>
#include <vector>

#include "JZCodeTools.h"     // JZVirtualTerminalConfig / TrustedProgram / DenyReason
#include "JZObject.h"
#include "JZResult.h"        // run() 返回 JZResult<std::string>(成功=run_id)
#include "JZShell/JZShell.h"         // 执行器: 自研解释器(cwd/env/变量跨命令保留)
#include "JZShell/JZShellAudit.h"   // 审核视图(由解释器自己的解析器产出, 只出结构不执行)
#include "JZThread.h"        // 每终端一个执行线程(见类头)

// ---------------------------------------------------------------------------
// JZVirtualTerminal —— 单台终端的执行面与审核面
//
// 设计(2026-09-17 调整): 一个 JZVirtualTerminal 就是"一个终端",由上层
// (如 JZAgentNode 用 map<session, JZVirtualTerminal> 管理)负责创建/销毁与
// 按会话路由。本类**不再维护多会话**: cwd / 环境变量 / 在跑进程 / 运行记录
// 都是"这一个终端"自己的状态。
//
// 为什么收成一个类: 任何"会碰这台机器"的动作都从这里走 —— 先审核, 再执行。
// 两块职责: 审核(checkRead/checkWrite/checkExec) / 裸命令口子 run()(交给 shell)。
// ⚠ 审核**引擎已搬走**(2026-11-09): 白名单/区域/危险参数/安全底线都在 JZShellPolicy 里,
//   由本类的 JZShell 持有; 这里的 check*() 只是**转发**(`m_shellImpl->policy().xxx`),
//   自身不再有 m_config / m_programs / m_trustedPrograms。为什么要搬: 谁执行谁审 ——
//   JZShell 拿到的是展开后的 argv, 而且任何用 JZShell 的宿主(CLI/测试/远端)都自动受管。
// 内建文件动作(ls/read/write/edit/mkdir/rm/cp/mv)已删除: 模型侧读写改文件走
// JZReadFileTool/JZWriteFileTool/JZModifyFileTool(带行号/分段/唯一匹配), ls/cp/mv/rm
// 走 terminal_run 交给 shell; 这里只留下 cwd 的读写入口(changeDir/printDir)。
//
// ⛔ 参数一律结构化, 绝不拼命令行字符串。
// 执行(2026-11-09): 底层换成 **JZShell(自研解释器)**, 且每个终端自带一个 **JZThread**:
//
//     JZVirtualTerminal  ── 1:1 ──  JZThread  ── 1:1 ──  JZShell
//
//   因此这是一个**真有状态的 shell**: cwd / export / 变量 / 函数都在同一个解释器实例里,
//   跨命令保留(`cd /tmp` 之后下一条就在 /tmp)。线程是惰性起来的: 第一次 run() 才
//   `new JZThread + start + moveToThread`, 终端回收时 quit+wait。
//
//   为什么必须"一个 shell 一个线程":
//     1) JZShell 不加锁, 只能被固定线程访问;
//     2) JZProcess 的异步信号要在有事件循环的线程上派发 —— JZThread 就是那个循环。
//
// 并发(2026-11-09 收紧): **一个终端同时只能跑一条前台命令**。第二条直接报错(带正在跑的
//   那条原文), 不排队 —— 排队会让调用方以为"已经在跑了", 而 cwd 还停在上一条中途。
//   要并发就多开终端(上层 new 几个 JZVirtualTerminal, 各自一个线程)。
//
// 本类不是 JZAgentTool —— 是否暴露、以什么名字暴露由上层决定
// (本地 code_agent 直接持有; 远端 agent_node 经 JZTerminalRegistry 包成 MCP 工具)。
//
// ⚠ "都走 shell": 解析/展开/执行全在 JZShell 里; 这里的 JZShellParser 只做**审核**
//   (先按结构过白名单/区域/危险参数, 再交给 JZShell 跑)。
// ---------------------------------------------------------------------------
class JZVirtualTerminal : public JZObject
{
public:
    // ---- 审核结论 ----
    // 类型本体在 JZVirtualTerminalConfig.h(PolicyDecision / PolicyVerdict), 引擎在
    // JZShellPolicy.h。这里保留别名, 现有写法 `JZVirtualTerminal::Verdict` /
    // `::Decision::Deny` 一行都不用改。
    using Decision = PolicyDecision;
    using Verdict = PolicyVerdict;

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

    // ==================== 策略(审核依据): 全部转发给 JZShellPolicy ====================
    // 终端配置: 目录边界 + 权限分档 + 命令策略(见 JZVirtualTerminalConfig.h)。
    // 换工作区时会把 cwd 拉回工作区根(旧 cwd 可能已经失效)
    void setTerminalConfig(const JZVirtualTerminalConfig &config);
    const JZVirtualTerminalConfig &terminalConfig() const;

    // 显式指定的可信程序(全路径): 优先于在授信目录里按名搜索
    void setTrustedPrograms(const std::vector<TrustedProgram> &programs);
    const std::map<std::string, std::string> &trustedPrograms() const;

    // ⚠ 短名 -> 全路径(programPaths)已交给 JZShell 自己解析(见 JZShell::addProgramPath),
    //   终端不再提供 programPath()/expandProgramPaths()。

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

    // ==================== 审核: 唯一入口(转发到 JZShellPolicy) ====================
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

    // ==================== 终端目录(唯一的 cwd 入口) ====================
    // 没有这两个就没有"终端 cwd": run() 的审核基准(base)与每条命令的
    // setWorkingDirectory 都从这里取。内建文件动作已删除(见类头注释)。
    bool changeDir(const std::string &path, std::string &error);
    bool printDir(std::string &cwd) const;

    // ==================== 命令 ====================
    // 解析/展开/执行交给本终端自己的 JZShell(在 m_thread 上跑):
    //   `&& || ; |` 管道、`> >> <` 重定向、通配符、变量/命令替换、if/for/while、函数、子 shell。
    // ⭐ 单前台任务: 同一时刻只有一条命令。上一条没结束再调 → 直接报错(不排队)。
    // run() 自己**不阻塞**: 投递给执行线程后立即返回 run_id, 结果走 cb(在终端线程回调)。
    // 返回: 成功 = run_id(供 terminal_query / cancel 引用); 失败 = 原因
    // (命令被拒 / 上一条还在跑 —— 这时 cb 也会被同步调一次 ok=false)。
    // 超时: 透传给 JZShell 的**单条外部命令**超时; 前台硬上限 15 分钟,
    //       <=0 或超限时归一到 15 分钟。
    // ⚠ 没有 onChunk / background 参数: 解释器整条返回, 也没有后台任务。
    JZResult<std::string> run(const std::string &command, int timeoutMs, const RunCallback &cb);

    // 停掉**当前这一条**: 转成 JZShell::requestCancel()(协作式, 外部命令会被 kill)。
    // 因为执行在独立线程上, 这条会真生效: 下一次 100ms 分片检查就感知到。
    // 只有一条在跑, 所以不需要 run_id; 返回是否确实停了一条(0 条时 false)。
    bool cancel();
    // 有没有命令在跑(单前台任务, 所以是 0/1)。
    bool isRunning() const { return !m_tasks.empty(); }
    // 在跑的那条的 run_id, 空闲时为空串(给 terminal_query / 展示用)。
    std::string activeRunId() const;

    // ==================== 输出: **不加工**(2026-10-14 起, 过滤整套删除) ====================
    // 命令回包 = 本次运行的**原始合并输出**(stdout/stderr 按到达顺序), 没有"按重要性挑几行"、
    //   没有逐行 stdout:/stderr: 标签、没有头尾截断 —— **过滤交给模型自己**:
    //   grep / head / tail / sort / uniq / wc / sed / awk 都在程序白名单里(见 JZShellPolicy::buildDefaultPrograms
    //   的 Shell 段), Git Bash 也自带这些工具, 模型想少看就自己 `... | grep -i error | tail -20`。
    // 为什么删: 工具替模型判断"哪几行重要"时, 模型把"没看到"读成"输出丢了" ⇒ 重跑命令 + 反复
    //   terminal_query(实测 19 次重跑 + 64 次查询, 直接撞轮次上限)。
    // 只剩一个**防 OOM 的字节阀**(kMaxRunOutputBytes, 超了从头部裁); 完整日志由 terminal_query 取。
    //
    // 两个"阀"分工不同, 但都**不做重要性挑拣**(挑拣是以前那套过滤, 已删):
    //   · kMaxRunOutputBytes(8MB, **内存阀**): 累积输出超了就从头部丢, 防 OOM;
    //   · replyMaxBytes(32KB, **上下文阀**, 见 setReplyMaxBytes): 回包超了只回**末尾**,
    //     并在开头加**一行显式标记**(省略了多少字节 / 完整日志用什么取) —— 模型上下文比磁盘贵得多,
    //     而"完整日志"始终在运行记录里 ⇒ 这条截断是**可恢复**的, 不是静默丢内容。
    //     (与 terminal_query 的 32KB 回包上限同一口径, 免得两条路宽窄不一。)
    void setReplyMaxBytes(size_t n) { m_replyMaxBytes = n; }   // 0 = 不限制(宿主/测试用)
    size_t replyMaxBytes() const { return m_replyMaxBytes; }

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
    // 审核引擎的唯一入口: 解释器自带的那份 policy(见 JZShellPolicy.h)。
    // m_shellImpl 在构造时就建好, 所以 check*() 不会因为"还没跑过命令"而空指针;
    // ensureWorker() 只是把它 moveToThread 过去。
    JZShellPolicy &policy();
    const JZShellPolicy &policy() const;

    // 程序映射不再改写命令串: 短名 -> 全路径由 JZShell::resolveProgram() 在解析程序时做,
    // 白名单补登记在 ensureWorker() 里一次性完成。

    // ⚠ 不再有 pid 归属判定: 外部命令由 JZShell 自己起/收, 终端拿不到 pid ⇒ kill 一律拒,
    //   停自己的命令统一走 cancel()(terminal_cancel)。

    // 一次运行的记录。stdout/stderr 分存, 可按流/关键字过滤。
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
        std::string output;                 // 本次运行的累积输出(合并 stdout/stderr)
        bool outputTruncated = false;       // output 超过字节上限被裁过
        int64_t startTime = 0;
        RunCallback cb;
    };

    // run() 的实现(审核 + 起进程): 失败时填 error 返回 false, 成功时回填 runId。
    // 拆出来是因为中间几十处 Deny 都是"填 error + return false", 包一层比逐处改成 JZResult 干净。
    bool startRun(const std::string &command, int timeoutMs, const RunCallback &cb, std::string &error,
                  std::string *runId);

    RunTask *taskOf(const std::string &runId);
    const RunTask *taskOf(const std::string &runId) const;
    void eraseTask(const std::string &runId);
    // 一次运行结束的收尾(唯一路径): 记结果 -> 摘任务 -> 回调, 全部按 runId 走。
    // 由执行线程 post 回来, 在终端自己的线程上执行(不许在 worker 里直接调)。
    void finishRun(const std::string &runId, int exitCode, const std::string &out,
                   const std::string &err, bool canceled, const std::string &cwd);

    // 惰性创建执行线程 + 把 JZShell moveToThread 过去; 回收/析构时 stopWorker()。
    bool ensureWorker(std::string &error);
    void stopWorker();

    void touch();                       // 刷新活跃时间 + 记审计
    // 把一块输出追加到该次运行(合并)与运行记录(逐行, 带流标签), 并按需推给它的流式回调
    void appendRunOutput(const std::string &runId, const std::string &stream, const std::string &chunk);

    void audit(const std::string &action, const Verdict &verdict, const std::string &detail);

    // 审核引擎不在这里: 白名单/区域/可信程序都在 JZShell::policy() 里(见 JZShellPolicy.h)

    // ---- 单终端状态(原 Session 的字段提到这里) ----
    std::string m_id;                   // 会话 key(上层设)
    std::string m_cwd;                  // 规范化后的绝对目录
    std::map<std::string, std::string> m_env;   // 终端级环境变量覆盖
    int64_t m_lastActive = 0;

    // ---- 在途运行(一次 run() 一条) ----
    // 每条各自一个 JZProcess + JZTimer, 所有权在终端(this), 结束/析构时 kill + 回收。
    std::vector<RunTask> m_tasks;
    // 取消请求(主线程写, 执行线程读): 用来补 JZShell::exec 入口重置标志的那个窗口 ——
    // "刚投递还没进 exec"时收到的取消不能丢。
    std::atomic<bool> m_cancelPending{ false };

    std::string m_shell;                // 只作展示名(见 shellProgram()); 执行器是 m_shellImpl
    JZThread *m_thread = nullptr;       // 本终端的执行线程(惰性创建, 回收时 quit+wait)
    JZShell *m_shellImpl = nullptr;     // 本终端的解释器(审核引擎也在里面); 构造即建, 之后归 m_thread
    bool m_shellAttached = false;       // 解释器是否已 moveToThread(m_thread) 并灌好初始状态
    int m_recycleMs = 15 * 60 * 1000;
    size_t m_replyMaxBytes = 32 * 1024; // 单次回包的字节上限(上下文阀, 见 setReplyMaxBytes)
    int m_runIdGen = 0;

    // 运行记录(后台/流式): runId -> 记录, 发生顺序另用 m_runOrder 保序
    std::map<std::string, RunRecord> m_runs;
    std::vector<std::string> m_runOrder;

    AuditCallback m_audit;
};

#endif // !JZ_VIRTUAL_TERMINAL_H_
