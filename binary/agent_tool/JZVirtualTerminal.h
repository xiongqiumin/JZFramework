#ifndef JZ_VIRTUAL_TERMINAL_H_
#define JZ_VIRTUAL_TERMINAL_H_

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "JZCodeTools.h"   // ZoneConfig / ProgramPolicy / TrustedProgram / DenyReason
#include "JZObject.h"

class JZProcess;
class JZTimer;

// ---------------------------------------------------------------------------
// JZVirtualTerminal —— 这台机器上**唯一的执行面与审核面**
//
// 为什么收成一个类
//   本来"执行"散在三处: 文件工具自己开文件、JZShellTool 自己开进程、虚拟终端自己一套命令表;
//   于是"审核"也跟着散了三份(checkPathPermission / resolveArg / 常驻 shell 索性没有)。
//   现在统一: 任何"会碰这台机器"的动作都从这里走 —— **先审核, 再执行**。
//
// 三块职责
//   1) 会话外壳  id / 多会话 / 空闲回收 / 列举。每个会话绑一个后端 + 一个 cwd。
//   2) 审核      checkRead / checkWrite / checkExec 是**唯一入口**; 区域模型、程序白名单、
//                策略开关全在这里, 下面每个动作方法内部都先调它。
//   3) 动作      内建实现(不经 shell): 列目录 / 读 / 写 / 编辑 / 建目录 / 删 / 拷 / 移 / cd;
//                另有一个"裸命令"口子 run(), 交给后端。
//
// ⛔ 参数一律**结构化**(JSON / 参数数组), 绝不拼命令行字符串
//      write_file 的内容可能几 MB 且带换行、引号; modify_file 是纯文本操作;
//      更关键的是: 字符串拼装必然让"审核器看到的"和"shell 实际解析的"不一致,
//      那是最经典的注入面。
//
// ⚠ 命令由**我们**解释, 不挂常驻 shell(2026-09-15 定的, 原先设想的 Shell 后端已去掉)
//      每条命令一个 JZProcess, 会话状态(cwd / env)由我们自己维护 ——
//      好处是状态可序列化、可审计, 而且 `a && b` 这种串联能**逐条审权限**,
//      交给 shell 就只能整串一起放行。
//      代价: 交互式程序(vim / top / REPL)做不了 —— 它们要 tty, 而这个模型里
//      子进程的 stdin/stdout 是管道。真要做得上 ConPTY, 那是另一个量级。
//
// 本类**不是** JZAgentTool —— 它只是执行/审核面。要不要暴露给模型、以什么名字暴露,
// 由上层决定(本地 code_agent 直接持有; 远端 agent_node 再包成 MCP 工具)。
//
// 当前状态(2026-09-15): 会话 / 审核 / 内建动作 / run 都已实现。
//   ⚠ 平台定位: **只跑 Linux, 不追求 Windows 支持**(目标机是 Linux)。
//   ⚠ "都走 shell"是定案: 管道/重定向/串联不自己解析, 整条命令交给 `/bin/sh -c`。
//     因此审核是**命令级**的(每段首 token 过白名单 + 重定向目标过写权限), 审不到
//     命令替换内部和被调用程序自己的行为 —— 详见 .cpp 里"命令"那一节。
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

    // ---- 会话快照(给列表/状态用, 不暴露内部对象) ----
    // 注意会话状态是**我们自己维护的**(不是某个常驻 shell 的): cwd + 环境变量覆盖。
    struct SessionInfo
    {
        std::string id;
        std::string cwd;
        bool running = false;    // 有命令在途
        int64_t lastActive = 0;  // 毫秒时间戳
    };

    // 命令结果回调: ok=false 时 error 有原因(output 可能是已拿到的部分)
    typedef std::function<void(bool ok, const std::string &output, const std::string &error)> RunCallback;

    // 一条命令的退出结果(串起来跑 `&&` / `||` 时要用)
    struct CommandResult
    {
        int exitCode = -1;
        bool timeout = false;
        std::string output;
        std::string error;
    };

    explicit JZVirtualTerminal(JZObject *parent = nullptr);
    ~JZVirtualTerminal() override;

    // ==================== 策略(审核依据) ====================
    void setZoneConfig(const ZoneConfig &zone);
    const ZoneConfig &zoneConfig() const;

    void setProgramPolicy(const ProgramPolicy &policy);
    const ProgramPolicy &programPolicy() const;

    // 显式指定的可信程序(全路径): 优先于在授信目录里按名搜索
    void setTrustedPrograms(const std::vector<TrustedProgram> &programs);
    const std::map<std::string, std::string> &trustedPrograms() const;

    // 程序白名单。构造时会装一份默认表(git/tsc/node/cmake 归 Tool;
    // pip/npm/conda 归 PackageManager; curl/wget 归 Network; reg/sc/net/setx 归 SystemAdmin),
    // 需要就追加。分类能不能放行由 ProgramPolicy 决定(SystemAdmin 永不放行)。
    void addProgram(const std::string &name, std::vector<std::string> exeNames, CmdCategory category,
                    std::vector<std::string> denyFlags = std::vector<std::string>(),
                    std::vector<std::string> denyModules = std::vector<std::string>());
    // 当前白名单里的程序名(供提示词/自检)
    std::vector<std::string> availablePrograms() const;

    // 跑命令用的 shell。命令字符串是**整条**交给它的(我们不解析管道/重定向/串联)。
    // 自动探测: POSIX = /bin/sh; Windows = Git for Windows 的 Git/bin/bash.exe
    // (Windows 的 PATH 里没有 ls/cat/grep, 只有 Git Bash 那份, 而且与 Linux 同名)。
    // ⛔ Windows 上**不退回 cmd.exe**: 找不到就返回空串, run() 会明确报错。
    // 宿主可以显式覆盖(例如节点启动参数 --shell)。
    void setShellProgram(const std::string &program);
    std::string shellProgram() const { return m_shell; }

    // ==================== 审核: 唯一入口 ====================
    // 所有动作方法内部都先调这三个, 调用方也可以先问一遍(例如 Ask 档先弹框)。
    // 收的都是**已经解析过的绝对路径**; 相对路径那一步是 resolvePath 的活。
    Verdict checkRead(const std::string &path) const;
    Verdict checkWrite(const std::string &path) const;
    // 带上参数是为了查白名单里的 denyFlags / denyModules(如 `git -C`、`python -m pip`)
    Verdict checkExec(const std::string &program) const;
    Verdict checkExec(const std::string &program, const std::vector<std::string> &args) const;

    // 解析一个路径参数: 相对路径按会话 cwd 拼, 然后按 writeAccess 审核。
    // 返回 false 时 verdict 里带 DenyReason, error 里有可读原因。
    bool resolvePath(const SessionInfo &session, const std::string &path, bool writeAccess,
                     std::string &absPath, Verdict &verdict, std::string &error) const;

    // ==================== 会话 ====================
    // cwd 为空时用工作区根; 返回新会话 id(如 "t1"), 失败返回空串。
    std::string openSession(const std::string &cwd, std::string &error);
    bool closeSession(const std::string &id);
    std::vector<SessionInfo> sessions() const;
    bool sessionInfo(const std::string &id, SessionInfo &out) const;

    // 空闲多久回收(毫秒), 默认 15 分钟; <=0 表示不回收。
    // 判定"空闲"= 既没被调用过、也没有输出, 且**没有命令在途**。
    void setRecycleMs(int ms) { m_recycleMs = ms; }
    int recycleMs() const { return m_recycleMs; }

    // ==================== 动作(内建, 不经 shell; 内部先审核) ====================
    bool listDir(const std::string &sessionId, const std::string &path,
                 std::vector<std::string> &names, std::string &error);
    bool readFile(const std::string &sessionId, const std::string &path,
                  std::string &text, std::string &error);
    bool writeFile(const std::string &sessionId, const std::string &path,
                   const std::string &text, bool append, std::string &error);
    // oldStr 为空 = 整文件覆盖; 否则精确替换, replaceAll=false 时要求唯一匹配
    bool editFile(const std::string &sessionId, const std::string &path,
                  const std::string &oldStr, const std::string &newStr,
                  bool replaceAll, std::string &error);
    bool makeDir(const std::string &sessionId, const std::string &path, std::string &error);
    bool removePath(const std::string &sessionId, const std::string &path,
                    bool recursive, std::string &error);
    bool copyPath(const std::string &sessionId, const std::string &from,
                  const std::string &to, std::string &error);
    bool movePath(const std::string &sessionId, const std::string &from,
                  const std::string &to, std::string &error);
    bool changeDir(const std::string &sessionId, const std::string &path, std::string &error);
    bool printDir(const std::string &sessionId, std::string &cwd) const;

    // ==================== 命令 ====================
    // 语法由**我们**解析(不是 shell): 支持 `&&` `||` `;` 串联(逐条审核)、
    // `|` 管道、`>` `>>` `<` 重定向、通配符与变量展开。不支持的写法会明确报错。
    //
    // 超时后先按 ok=false 应答一次, 命令不打断, 之后用 takeOutput 取增量。
    bool run(const std::string &sessionId, const std::string &command,
             int timeoutMs, const RunCallback &cb, std::string &error);
    bool cancel(const std::string &sessionId);
    std::string takeOutput(const std::string &sessionId);

    // ==================== 会话状态(我们自己维护, 不是某个 shell 的) ====================
    // 环境变量: 会话级覆盖(等价于 shell 里的 set/export)。命令进程起的时候
    // 拿"进程环境 + 会话覆盖"拼一个完整环境块传下去 —— 必须给完整块, 否则 PATH 会丢。
    bool setEnv(const std::string &sessionId, const std::string &name,
                const std::string &value, bool persistent);
    bool unsetEnv(const std::string &sessionId, const std::string &name);
    std::map<std::string, std::string> env(const std::string &sessionId) const;

    // ==================== 审计(预留) ====================
    // 每个动作审核通过或被拒都会回调一次; 远端节点用它落审计日志。
    typedef std::function<void(const std::string &action, const Verdict &verdict,
                               const std::string &detail)> AuditCallback;
    void setAuditCallback(const AuditCallback &cb) { m_audit = cb; }

protected:
    struct Session;

    Session *findSession(const std::string &id);
    const Session *findSession(const std::string &id) const;
    void reapIdle();              // 由回收定时器驱动
    void ensureReaper();          // 首次开会话时才建回收定时器(免得空转)
    void buildDefaultPrograms();  // 装默认程序白名单
    void touch(Session *session); // 刷新活跃时间 + 记审计

    // 审核通过/被拒都记一笔; action 是动词名(ls/read/write/...) —— 审计与统计都看它
    void audit(const std::string &action, const Verdict &verdict, const std::string &detail);

    // 把一个绝对路径按"读 or 写"审核, 通过则填 absPath
    bool checkPath(const std::string &sessionId, const std::string &path, bool writeAccess,
                   const std::string &action, std::string &absPath, std::string &error);

    ZoneConfig m_zone;
    ProgramPolicy m_policy;
    std::map<std::string, std::string> m_trustedPrograms;   // 小写程序名 -> 全路径
    std::map<std::string, ProgramSpec> m_programs;          // 小写程序名 -> 白名单项

    std::map<std::string, Session *> m_sessions;
    std::string m_shell;                  // 见 shellProgram(): 构造时自动探测
    int m_recycleMs = 15 * 60 * 1000;
    int m_idGen = 0;
    JZTimer *m_reapTimer = nullptr;

    AuditCallback m_audit;
};

#endif // !JZ_VIRTUAL_TERMINAL_H_
