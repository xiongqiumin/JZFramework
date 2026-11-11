#ifndef JZ_SHELL_POLICY_H_
#define JZ_SHELL_POLICY_H_

#include <map>
#include <string>
#include <vector>

#include "JZShellAudit.h"              // jzsh::AuditCommand(底线判定按结构走)
#include "JZVirtualTerminalConfig.h"   // JZVirtualTerminalConfig / ProgramSpec / PolicyVerdict ...

// ---------------------------------------------------------------------------
// JZShellPolicy —— 审核引擎(策略与判定), 从 JZVirtualTerminal 原样搬过来
//
// 为什么单独一层: 审核本来就是**执行前的闸**, 和"谁来执行"绑在一起才对 —— 谁执行谁
// 自己审, 宿主(终端/CLI/远端 agent)只负责配策略。所以引擎归 JZShell 所有, 本类只装
// 判定逻辑, 不碰 JZObject/线程/信号:
//
//   JZShell ── 持有 ──> JZShellPolicy   (白名单 / 区域 / 危险参数 / 安全底线)
//   JZVirtualTerminal ── 转发 ──> JZShell::policy()   (兼容旧调用点, 自身不再有策略)
//
// 不是安全边界(白名单里有 g++ 就等于能执行任意代码), 管的是副作用收敛与防误操作;
// 真正的边界只能是沙箱/容器。已知盲区: 看不见的命令($( ) / 反引号 / 子 shell 内容 /
// heredoc body)按可信处理, 见 JZShellParser.h 顶部注释。
//
// ⚠ 本类是**纯数据 + 纯函数**, 没有加锁: 配置(setConfig/addProgram)应当在开跑之前做完;
//   运行期只读。想并发就得一个 shell 一个 policy 实例。
// ---------------------------------------------------------------------------
class JZShellPolicy
{
public:
    using Decision = PolicyDecision;
    using Verdict = PolicyVerdict;

    JZShellPolicy();   // 装默认程序白名单(见 buildDefaultPrograms)

    // ==================== 策略(审核依据) ====================
    // 目录边界 + 权限分档 + 命令策略(见 JZVirtualTerminalConfig.h)。
    void setConfig(const JZVirtualTerminalConfig &config);
    const JZVirtualTerminalConfig &config() const { return m_config; }

    // 相对路径的默认基准(执行器当前的 cwd)。判定时传了 base 就优先用 base。
    void setBaseCwd(const std::string &cwd) { m_baseCwd = cwd; }
    const std::string &baseCwd() const { return m_baseCwd; }

    // 显式指定的可信程序(全路径): 优先于在授信目录里按名搜索
    void setTrustedPrograms(const std::vector<TrustedProgram> &programs);
    const std::map<std::string, std::string> &trustedPrograms() const { return m_trustedPrograms; }

    // 程序白名单。构造时会装一份默认表。
    void addProgram(const std::string &name, std::vector<std::string> exeNames, CmdCategory category,
                    std::vector<std::string> denyFlags = std::vector<std::string>(),
                    std::vector<std::string> denyModules = std::vector<std::string>());
    std::vector<std::string> availablePrograms() const;   // 供提示词/自检
    void buildDefaultPrograms();                          // 装默认表(构造时调)

    // ==================== 审核: 唯一入口 ====================
    Verdict checkRead(const std::string &path) const;
    Verdict checkWrite(const std::string &path) const;
    // 只要一份 config 就能判的"这个路径能不能读/写"(文件工具用)。
    // 它不依赖程序白名单/可信程序, 所以宿主(如 JZVirtualTerminal)只存一份
    // JZVirtualTerminalConfig 就够了, 不必为了文件审核再new一个 policy 对象;
    // "执行前审"(checkExec/dangerousArg/安全底线)才需要 policy, 那份在 JZShell 里。
    static Verdict checkReadIn(const JZVirtualTerminalConfig &cfg, const std::string &path);
    static Verdict checkWriteIn(const JZVirtualTerminalConfig &cfg, const std::string &path);
    Verdict checkExec(const std::string &program) const;
    // base: 审核时解析相对路径用的基准目录, 空 = 用 baseCwd()(再空则工作区根)。
    // `cd X && cmd` 这种, shell 会真的 chdir 过去, 静态检查必须跟着走进去,
    // 否则审的是工作区里的路径、实际写的却是 X 里的 —— 审核就成了摆设。
    Verdict checkExec(const std::string &program, const std::vector<std::string> &args,
                      const std::string &base = std::string()) const;

    // 解析一个路径参数: 相对路径按 base(默认 baseCwd) 拼, 然后按 writeAccess 审核。
    bool resolvePath(const std::string &path, bool writeAccess,
                     std::string &absPath, Verdict &verdict, std::string &error,
                     const std::string &base = std::string()) const;

    // ==================== 细粒度判定(暴露给执行器/自检) ====================
    // `rm -rf /`、`dd of=/dev/sda` 这类: 单看程序名审不出来(rm/dd 本身在白名单里),
    // 得看参数组合。返回 nullptr 表示用法没问题。
    const char *dangerousArgReason(const std::string &program,
                                   const std::vector<std::string> &args,
                                   const std::string &base = std::string()) const;

    // 安全底线: 即便 AccessLevel::Unrestricted(全部不检查)也始终生效, 执行之前最后一道闸。
    // 覆盖两类永远不许的:
    //   1) 特别危险的指令(关机 / 格式化磁盘 / 往块设备写 ...)
    //   2) 删除工作区外的文件(rm / rmdir 的目标不在工作区树内 —— 含根目录本身)
    // 返回 Allow 表示通过底线(其余检查在不限制档里本来就跳过)。
    const char *criticalDangerReason(const std::string &program,
                                     const std::vector<std::string> &args) const;
    bool isDeleteCommand(const std::string &programLower) const;   // "rm"/"rmdir"...
    bool isWithinWorkspace(const std::string &absPath) const;      // 相对工作区根
    Verdict checkCriticalFloor(const jzsh::AuditCommand &cmd, const std::string &base) const;

private:
    // base 为空时取 baseCwd(), 再空就退回工作区根 —— 原来散在各处的三目表达式收在这里。
    std::string effectiveBase(const std::string &base) const;

    JZVirtualTerminalConfig m_config;
    std::map<std::string, std::string> m_trustedPrograms;   // 小写程序名 -> 全路径
    std::map<std::string, ProgramSpec> m_programs;          // 小写程序名 -> 白名单项
    std::string m_baseCwd;                                  // 见 setBaseCwd
};

#endif // !JZ_SHELL_POLICY_H_
