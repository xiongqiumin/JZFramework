#include "JZVirtualTerminal.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <set>

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#endif

#include "JZByteArray.h"
#include "JZDateTime.h"
#include "JZDebug.h"
#include "JZFileInfo.h"
#include "JZProcess.h"
#include "JZShell/JZShellAudit.h"
#include "JZString.h"
#include "JZTimer.h"

using namespace std;

// 累积输出字节上限(防 OOM): 超了从头部裁, 只留末尾
static const size_t kMaxRunOutputBytes = 8 * 1024 * 1024;

// 输出**不做过滤**(2026-10-14 起, 过滤整套删除: isBuildCommand / isErrorLine /
// filterBuildOutput / truncateGeneric 都去掉了)。命令回包就是原始合并输出, 模型自己用
// grep/head/tail/sort/uniq... 去过滤(这些都在程序白名单里, 见 buildDefaultPrograms)。
// 这里只剩一个防 OOM 的字节阀 kMaxRunOutputBytes(见 appendRunOutput)。

// JZByteArray -> std::string
std::string bytesToString(const JZByteArray &data)
{
    return std::string(data.data(), data.size());
}

// ==========================================================================
// 小工具
// ==========================================================================

namespace
{

int64_t currentMs()
{
    return JZDateTime::currentMSecsSinceEpoch();
}

void appendBytes(string &dst, const JZByteArray &data)
{
    dst.append(data.data(), (size_t)data.size());
}

// 程序名归一(programKey)、"像不像路径"(looksLikePath)、toSlash 都随审核引擎搬到
// JZShellPolicy.cpp 了; 终端的 check*() 现在只是转发。


const char *detachReason(const string &head)
{
    static const char *kDetach[] = {
        "nohup", "setsid", "disown", "screen", "tmux", "at", "batch", "daemon", "bg", "fg", "jobs",
    };
    for (const char *cmd : kDetach)
    {
        if (head == cmd)
            return "会脱离进程管理(拿不到输出、杀不掉), 不支持";
    }
    return nullptr;
}

// shell 复合语句(for / while / if ...): 我们只按 && || ; | 拆段逐段审,
// 循环体/分支体内部的命令审不到(比如 `for f in *; do rm -rf $f; done`), 所以明确拒绝,
// 而不是让它们落到"程序不在白名单里"这种看不懂的报错上。
const char *compoundReason(const string &head)
{
    static const char *kCompound[] = {
        "for", "while", "until", "if", "elif", "case", "select", "function",
    };
    for (const char *cmd : kCompound)
    {
        if (head == cmd)
            return "不支持 shell 复合语句(循环/分支): 里面的命令审不到。"
                   "请拆成多条命令, 或改用 xargs / 管道 / && 串联(如 `ls *.log | xargs rm`)";
    }
    return nullptr;
}

} // namespace

// ==========================================================================
// 构造 / 策略
// ==========================================================================

JZVirtualTerminal::JZVirtualTerminal(JZObject *parent)
    : JZObject(parent)
{
    // 解释器在构造时就建好(策略要能立刻配, setTerminalConfig 早于第一次 run)。
    // 线程亲和性在 ensureWorker() 里再处理(moveToThread) —— 之前没人执行, 只在本线程访问。
    m_shellImpl = new JZShell(this);
}

JZVirtualTerminal::~JZVirtualTerminal()
{
    stopWorker();
    m_tasks.clear();
}

void JZVirtualTerminal::setTerminalConfig(const JZVirtualTerminalConfig &config)
{
    // ① 本类留一份 config: 文件工具的"路径能不能读写"靠它判定(不需要 policy 对象)
    m_config = config;
    // ② 执行前审的那份 policy 在 shell 里, 同样按这份 config 配(白名单补登记/全路径登记
    //    都在 JZShellPolicy::setConfig 里完成)
    JZShellPolicy &p = policy();
    p.setConfig(config);

    // 配置里的基础环境(见 JZVirtualTerminalConfig::env): 导出给子进程。
    // ⚠ 以前终端只把 env 存在 m_env 里, 而 JZShell::setEnvVar 不 export ⇒ 子进程根本看不到。
    for (const auto &kv : config.env)
    {
        if (kv.first.empty())
            continue;
        m_env[kv.first] = kv.second;
        if (m_shellImpl)
            m_shellImpl->addExportedEnv(kv.first, kv.second);
    }

    // 目录基准变了, cwd 可能失效 —— 拉回工作区根(而不是让它悬着)
    if (m_cwd.empty())
    {
        m_cwd = m_config.zone.workspace;
        p.setBaseCwd(m_cwd);
    }
}

const JZVirtualTerminalConfig &JZVirtualTerminal::terminalConfig() const
{
    return m_config;
}

// ==========================================================================
// 审核: 唯一入口 —— 全部转发给解释器里的 JZShellPolicy
//   引擎(白名单 / 区域 / 危险参数 / 安全底线)不在本类里, 见 JZShellPolicy.h。
//   这里保留同名同签名的接口, 只是为了让工具层/旧调用点一行都不用改。
// ==========================================================================

JZVirtualTerminal::Verdict JZVirtualTerminal::checkRead(const string &path) const
{
    // 只靠 config: 文件工具读文件时也要问, 它和"跑命令"无关(见类头说明)
    return JZShellPolicy::checkReadIn(m_config, path);
}

JZVirtualTerminal::Verdict JZVirtualTerminal::checkWrite(const string &path) const
{
    return JZShellPolicy::checkWriteIn(m_config, path);
}

JZVirtualTerminal::Verdict JZVirtualTerminal::checkExec(const string &program) const
{
    // 单看程序名的放行判断: 基准用本终端 cwd(空则引擎退回工作区根)
    return policy().checkExec(program, vector<string>(), m_cwd);
}

JZVirtualTerminal::Verdict JZVirtualTerminal::checkExec(const string &program,
                                                       const vector<string> &args,
                                                       const string &base) const
{
    return policy().checkExec(program, args, base.empty() ? m_cwd : base);
}

bool JZVirtualTerminal::resolvePath(const string &path, bool writeAccess,
                                    string &absPath, Verdict &verdict, string &error,
                                    const string &base) const
{
    return policy().resolvePath(path, writeAccess, absPath, verdict, error,
                                base.empty() ? m_cwd : base);
}

// 执行前审的那份 policy: 在 shell 里(谁执行谁审)。
// 文件工具的**路径**判定不走这里, 用 m_config + checkReadIn/checkWriteIn(见类头)。
JZShellPolicy &JZVirtualTerminal::policy()
{
    return m_shellImpl->policy();
}

const JZShellPolicy &JZVirtualTerminal::policy() const
{
    return m_shellImpl->policy();
}

// ==========================================================================
// 终端状态
// ==========================================================================

JZVirtualTerminal::SessionInfo JZVirtualTerminal::info() const
{
    SessionInfo out;
    out.id = m_id;
    out.cwd = m_cwd;
    out.running = !m_tasks.empty();
    out.runningCount = (int)m_tasks.size();
    out.lastActive = m_lastActive;
    return out;
}

void JZVirtualTerminal::touch()
{
    m_lastActive = currentMs();
}

// ==========================================================================
// 终端状态: 环境变量
//
// 只做存储与查询 —— 真正注入子进程要等 JZProcess 支持 setEnvironment()。
// ==========================================================================

bool JZVirtualTerminal::setEnv(const string &name, const string &value, bool persistent)
{
    if (name.empty())
        return false;

    // Windows 环境变量名大小写不敏感: 覆盖时按不敏感匹配, 并保留原有写法
    string key = name;
    for (const auto &item : m_env)
    {
        if (str_toLower(item.first) == str_toLower(name))
        {
            key = item.first;
            break;
        }
    }

    if (persistent)
        m_env[key] = value;
    else
    {
        // 只对下一条命令生效: 记在"临时覆盖"里, run() 用完即清。
        // 目前 run 还没接, 先按持久处理并留个记号, 免得静默丢语义。
        m_env[key] = value;
        jzWarning() << "setEnv(persistent=false) 暂按持久处理(run 尚未完全接入临时覆盖)";
    }
    // 立刻同步给解释器(导出): 解释器已起时也生效, 不必等下一次 attach
    if (m_shellImpl)
        m_shellImpl->addExportedEnv(key, value);

    touch();
    return true;
}

bool JZVirtualTerminal::unsetEnv(const string &name)
{
    for (auto it = m_env.begin(); it != m_env.end(); ++it)
    {
        if (str_toLower(it->first) == str_toLower(name))
        {
            m_env.erase(it);
            touch();
            return true;
        }
    }
    return false;
}

map<string, string> JZVirtualTerminal::env() const
{
    return m_env;
}

// ==========================================================================
// 审计
// ==========================================================================

void JZVirtualTerminal::audit(const string &action, const Verdict &verdict, const string &detail)
{
    if (!m_audit)
        return;

    switch (verdict.decision)
    {
    case Decision::Allow: m_audit(action, verdict, detail); break;
    case Decision::Deny:  m_audit(action, verdict, verdict.detail); break;
    default:              m_audit(action, verdict, detail); break;
    }
}

// ==========================================================================
// 终端目录(唯一的 cwd 入口)
// ==========================================================================

bool JZVirtualTerminal::changeDir(const string &path, string &error)
{
    touch();

    Verdict verdict;
    string abs;
    if (!resolvePath(path.empty() ? "." : path, false, abs, verdict, error))
    {
        audit("cd", verdict, path);
        return false;
    }

    JZFileInfo target(abs);
    if (!target.isDir())
    {
        error = "不是目录: " + abs;
        return false;
    }

    audit("cd", verdict, abs);
    m_cwd = abs;
    return true;
}

bool JZVirtualTerminal::printDir(string &cwd) const
{
    cwd = m_cwd;
    return true;
}

// ==========================================================================
// 命令 —— 都交给 shell
// ==========================================================================

// 对外入口: 成功 = 已开始, 失败 = 原因。审核/起进程在 startRun() 里 —— 中间几十处 Deny 都是
// "填 error + return false" 的老写法, 包一层比逐处改成 JZResult 干净。
JZResult<void> JZVirtualTerminal::run(const string &command, int timeoutMs, const RunCallback &cb)
{
    // 前台任务硬上限 15 分钟: 防止调用方传过大的 timeout 让命令卡死太久;
    // 传 <=0(不设时)也归到这个上限, 不允许无限前台。
    static const int kMaxForegroundTimeoutMs = 15 * 60 * 1000;
    if (timeoutMs <= 0 || timeoutMs > kMaxForegroundTimeoutMs)
        timeoutMs = kMaxForegroundTimeoutMs;
    string error;
    if (!startRun(command, timeoutMs, cb, error))
        return JZResult<void>::Error(error);
    return JZResult<void>::Ok();
}

bool JZVirtualTerminal::startRun(const string &command, int timeoutMs, const RunCallback &cb, string &error)
{
    if (command.empty())
    {
        error = "命令为空";
        return false;
    }
    // 单前台任务: 上一条还没结束, 这一条直接拒(不排队 —— 见类头"并发"那段)
    if (!m_tasks.empty())
    {
        error = "这个终端同时只能跑一条命令: `" + m_tasks.front().command +
                "` 还在跑。等它结束(或用 terminal_cancel 停掉)再来";
        if (cb)
            cb(false, string(), error);
        return false;
    }

    touch();

    // 策略(工作区 / 权限档 / 白名单)都在解释器的 policy 里, 这里只是取一份只读引用。
    const JZVirtualTerminalConfig &cfg = policy().config();

    // ---- 审核: 解析成语法树 -> 逐条审 ----
    // 以前是字符串扫描 + 各种特判(& 是不是后台、2>&1 是不是重定向), 现在统一由
    // JZShell 出结构, 这里只负责按结构过策略。
    // 解析口径 = "看得见才审, 看不见按可信处理"(见 JZShell::parse 的 trustUnknown):
    // heredoc body / $( ) / 反引号 / 进程替换 / 子 shell 的**内容**整段跳过(已知盲区),
    // 审核层只会看到能解析出来的命令 —— 下面这些检查(白名单 / 区域 / 危险参数 / 底线)
    // 因此都只管"看得见的那部分"。
    // 两个等级的区别只在**检查项多少**(Unrestricted 跳过全部检查), 不在解析口径 ——
    // 所以这里只有一次 parse(以前按等级分两套调用, 现在统一)。
    jzsh::AuditLine parsed;
    string perr;
    if (!jzsh::auditParse(command, parsed, perr))
    {
        // 剩下的失败只有两类: ① "值不可知"(程序名 / 重定向目标带 $ ~ * ?);
        // ② 语法本身不完整(重定向没目标、引号不配对)。交给 shell 也只会报一句
        // 更难懂的错, 所以保守拒绝 + 给可读原因。
        Verdict v;
        v.decision = Decision::Deny;
        v.reason = DenyReason::CommandDenied;
        v.detail = perr;
        audit("exec", v, command);
        error = perr;
        return false;
    }
    // `cmd &` 后台: 放行(以前是以"脱离管理"为由拒绝的)。shell 会把这类任务的输出
    // 落到日志文件并把路径打出来(见 JZShell::spawnBackground), 所以"拿不到输出"这个
    // 前提不再成立; 它本来也不该占着前台等 timeout —— 常驻服务永远等不到头。
    // 代价: 后台进程不在本终端的 timeout 约束内(bash 同样如此), 需要时按 pid 杀。

    // 终端是有状态的(JZShell 常驻): 单独的 `cd` / `export` 现在**有效** ——
    // 它改的是这个终端解释器的状态, 下一条命令看得见。以前那两条"没有意义/不跨命令保留"
    // 的拒绝(前提是"每条命令一个新 shell")随执行模型一起删掉了。

    // 审核基准: 从终端当前 cwd 出发。遇到 `cd X && ...` 要跟着走进 X —— 解释器是真的会
    // chdir 过去的, 后面那些相对路径是相对 X 的; 静态检查不跟着走, 就会出现"审的是
    // 工作区里的 f、实际写的是 X/f", 写保护直接被绕过去。
    string base = m_cwd.empty() ? cfg.zone.workspace : m_cwd;

    for (size_t pi = 0; pi < parsed.pipelines.size(); pi++)
    {
        const jzsh::AuditPipeline &pl = parsed.pipelines[pi];

        // 单条(非管道)的 cd: 只改这一条命令的基准, 不改终端状态。
        // 管道里的 cd 在子 shell 里跑, 影响不到后面的段, 所以不算数。
        if (pl.commands.size() == 1 && !pl.commands[0].argv.empty()
            && pl.commands[0].argv[0] == "cd")
        {
            const jzsh::AuditCommand &cd = pl.commands[0];
            string target = (cd.argv.size() >= 2) ? cd.argv[1] : string();
            if (target.empty())
                target = base;

            if (target.find_first_of("$~*?") != string::npos)
            {
                Verdict v;
                v.decision = Decision::Deny;
                v.reason = DenyReason::CommandDenied;
                v.detail = "cd 的目标里有变量/通配符, 静态审不出是哪个目录: " + target +
                           "。请写成确定的目录";
                audit("cd", v, target);
                error = v.detail;
                return false;
            }

            string abs;
            Verdict pv;
            string cderr;
            if (!resolvePath(target, false, abs, pv, cderr, base))
            {
                audit("cd", pv, target);
                error = "cd 的目标不可读: " + target + " (" + cderr + ")";
                return false;
            }
            audit("cd", pv, abs);
            base = abs;
            continue;
        }

        for (const jzsh::AuditCommand &cmd : pl.commands)
        {
            string head = cmd.argv.empty() ? string() : cmd.argv[0];
            if (head.empty())
                continue;   // 只有 VAR=val 前缀的一段(如 `FOO=bar && cmd`), 没有程序可调

            // 不限制档(Unrestricted): 跑完"安全底线"就放行, 其余检查全跳过。
            if (cfg.unrestricted())
            {
                Verdict fv = policy().checkCriticalFloor(cmd, base);
                audit("floor", fv, head);
                if (!fv.allowed())
                {
                    error = "命令被拒绝(安全底线): " + head +
                            (fv.detail.empty() ? string() : " (" + fv.detail + ")");
                    return false;
                }
                continue;   // 跳过白名单 / 区域 / 危险参数 / 重定向的全部检查
            }

            if (cmd.programExpanded)
            {
                Verdict v;
                v.decision = Decision::Deny;
                v.reason = DenyReason::CommandDenied;
                v.detail = "程序名带了变量/通配符展开, 审不出到底要跑哪个程序: " + head +
                           "。请写成确定的程序名";
                audit("exec", v, head);
                error = v.detail;
                return false;
            }

            if (const char *why = detachReason(head))
            {
                Verdict v;
                v.decision = Decision::Deny;
                v.reason = DenyReason::CommandDenied;
                v.detail = string("命令被拒绝: ") + head + " —— " + why;
                audit("exec", v, head);
                error = v.detail;
                return false;
            }

            if (const char *why = compoundReason(head))
            {
                Verdict v;
                v.decision = Decision::Deny;
                v.reason = DenyReason::CommandDenied;
                v.detail = string("命令被拒绝: ") + head + " —— " + why;
                audit("exec", v, head);
                error = v.detail;
                return false;
            }

            // 带上参数审: 光看程序名不够 —— `python -c ...`、`git -C ...`、`rm -rf /`
            // 这些危险都藏在参数里
            Verdict v = checkExec(head, cmd.argv, base);
            audit("exec", v, head);
            if (!v.allowed())
            {
                error = "命令被拒绝: " + head +
                        (v.detail.empty() ? string() : " (" + v.detail + ")");
                return false;
            }

            // 重定向: fd 复制(2>&1 / >&-)不碰文件, 其余按读/写审路径
            for (const jzsh::AuditRedirect &r : cmd.redirects)
            {
                if (r.fdDup)
                    continue;

                string abs;
                Verdict pv;
                string rerr;
                if (!resolvePath(r.target, !r.read, abs, pv, rerr, base))
                {
                    audit("redirect", pv, r.target);
                    error = (r.read ? string("重定向来源不可读: ") : string("重定向目标不可写: ")) +
                            r.target + " (" + rerr + ")";
                    return false;
                }
                audit("redirect", pv, abs);
            }
        }
    }

    // 审核过了: 配了 programPaths 映射的短程序名, 在这里换成全路径再交给 shell。
    // (必须在审完之后 —— 换完就是路径, 再拿去审走的是路径判定, 区外一律拒)
    string execCommand = command;   // 短名 -> 全路径 由 JZShell::resolveProgram() 解析

    // ---- 一条在途运行 = 一次 JZShell::exec(在终端自己的执行线程上跑) ----
    RunTask task;
    task.command = command;
    task.startTime = JZPlatform::getTickCountMs();
    task.cb = cb;

    if (!ensureWorker(error))
    {
        if (cb)
            cb(false, string(), error);
        return false;
    }

    // 先入列再投递(收尾时按"唯一那条"取回)
    m_tasks.push_back(task);
    // 上一条的取消请求不能串到这一条
    m_cancelPending.store(false);

    // 跑的是"换过短名"的串: 命令已按原文审过, 这里只把配了映射的程序名换成全路径
    // (报错里仍是原文 —— 那是用户写的那条)
    JZVirtualTerminal *self = this;
    int cmdTimeout = timeoutMs;
    runOnThread(m_thread, [self, execCommand, cmdTimeout]()
                {
                    // ======== 这一段在终端自己的执行线程上(只有它能碰 m_shellImpl) ========
                    JZShell *sh = self->m_shellImpl;
                    if (!sh)
                    {
                        runOnObject(self->guardPointer(), [self]()
                                    {
                                        self->finishRun(1, string(), "解释器没有初始化", false, self->m_cwd);
                                    });
                        return;
                    }

                    // 取消请求落在这个窗口(已投递、exec 还没进)时, exec 入口会把取消标志
                    // 重置掉 —— 这里先自检一次, 免得"刚发出去就被取消"的命令照样跑完。
                    if (self->m_cancelPending.exchange(false))
                    {
                        runOnObject(self->guardPointer(), [self]()
                                    {
                                        self->finishRun(130, string(), "命令已被取消", true, self->m_cwd);
                                    });
                        return;
                    }

                    sh->setCommandTimeoutMs(cmdTimeout);
                    JZShell::Result r = sh->exec(execCommand);
                    const string cwd = sh->cwd();
                    const bool canceled = (r.exitCode == 130 && sh->cancelRequested());

                    // ======== 回终端线程收尾(m_tasks / 回调都只在那个线程动) ========
                    runOnObject(self->guardPointer(), [self, r, canceled, cwd]()
                                {
                                    self->finishRun(r.exitCode, r.output, r.error, canceled, cwd);
                                });
                });

    return true;
}

bool JZVirtualTerminal::cancel()
{
    // 只有一条在跑: 转成解释器的协作式取消 —— 执行线程在下一次 100ms 分片检查时杀掉
    // 外部命令, 收尾仍走 finishRun 那一条路径。
    //
    // ⚠ 必须**直接**置位, 不能 runOnThread: 解释器线程这时正阻塞在 exec() 里,
    //   投过去的事件要等 exec 返回才会被处理, 等于永远取消不掉(实测: cancel 回了,
    //   命令照样跑完)。requestCancel() 本来就是给"另一个线程"直接调的。
    if (m_tasks.empty())
        return false;
    m_cancelPending.store(true);
    if (m_shellImpl)
        m_shellImpl->requestCancel();
    return true;
}

void JZVirtualTerminal::appendRunOutput(const string &chunk)
{
    if (chunk.empty() || m_tasks.empty())
        return;

    RunTask *task = &m_tasks.front();
    task->output += chunk;
    // 累积输出只保留末尾 kMaxRunOutputBytes 字节, 超了从头部裁掉, 避免长输出(编译巨量
    // 日志)把内存吃爆。
    if (task->output.size() > kMaxRunOutputBytes)
    {
        task->outputTruncated = true;
        task->output.erase(0, task->output.size() - kMaxRunOutputBytes);
    }
}

// 惰性创建执行线程, 并把构造时就建好的解释器挂上去。只能在终端线程上调(要做 moveToThread)。
bool JZVirtualTerminal::ensureWorker(string &error)
{
    (void)error;
    if (!m_thread)
        m_thread = new JZThread(this);
    if (!m_thread->isRunning())
        m_thread->start();

    if (!m_shellImpl)
        m_shellImpl = new JZShell(this);   // 回收后再跑: 解释器重建, 策略留在 policy 里(已随之重建)

    if (!m_shellAttached)
    {
        // 把终端**当前**状态交给解释器; 之后以解释器为准, 终端只做镜像
        if (!m_cwd.empty())
            m_shellImpl->setCwd(m_cwd);
        // 终端级 env 是"打算给命令用的", 所以要导出(只写 m_env 的话子进程看不见)
        for (auto &kv : m_env)
            m_shellImpl->addExportedEnv(kv.first, kv.second);
#ifdef _WIN32
        // Windows 上外部命令(ls/grep/sed...)来自 Git for Windows; 不扫 PATH,
        // 免得撞上 System32 的 bash.exe(WSL 启动器)
        const char *roots[] = { "C:/Program Files/Git", "C:/Program Files (x86)/Git" };
        for (const char *r : roots)
        {
            string usrBin = string(r) + "/usr/bin";
            if (JZFileInfo(usrBin).isDir())
            {
                m_shellImpl->addPathDir(usrBin);
                m_shellImpl->addPathDir(string(r) + "/bin");
                break;
            }
        }
#endif
        // 配置里的短名映射交给解释器: 之后 `tsc -p .` 直接在 resolveProgram 里解析成全路径,
        // 不再需要"审完再改写命令串"那一步。白名单/全路径登记见 setTerminalConfig()。
        for (const auto &kv : policy().config().programPaths)
        {
            string name = str_toLower(kv.first);
            if (name.empty() || kv.second.empty())
                continue;
            m_shellImpl->addProgramPath(name, kv.second);
        }

        // 线程亲和性: 之后这个解释器只归 m_thread 用(它不加锁)
        m_shellImpl->moveToThread(m_thread);
        m_shellAttached = true;
    }
    return true;
}

void JZVirtualTerminal::stopWorker()
{
    if (!m_thread)
        return;
    if (m_shellImpl)
    {
        JZShell *shell = m_shellImpl;
        m_shellImpl = nullptr;
        shell->requestCancel();        // 让在跑的外部命令尽快退出
        // ⚠ 解释器(以及它名下的 JZProcess)已经 moveToThread 到 m_thread: 删它必须回到
        //   **它自己的线程**上。JZObject 析构里的 clearGuardPointer() 会断言
        //   m_thread->id() == currentThreadId(), 在主线程 delete 一个 worker 对象 =
        //   __debugbreak / CRT assert(实测 0x80000003, 堆状态好时表现为进程退出码 3)。
        //   做法: 先摘父子关系(免得 ~JZObject 在主线程替我们删), 再 deleteLater 投给 worker;
        //   然后**等它真的删掉**再 quit —— 直接 quit 会把这条事件丢掉(对象泄漏)。
        shell->setParent(nullptr);
        JZObjectPointer guard = shell->guardPointer();
        shell->deleteLater();
        for (int i = 0; i < 200 && guard.object() != nullptr; i++)   // 最多等 2s
            JZThread::msleep(10);
    }
    m_thread->quit();
    m_thread->wait();
    m_thread = nullptr;
    m_shellAttached = false;
}

// 一次运行的唯一收尾路径: 自然结束 / 超时被杀 / cancel 都走这里。
// ⚠ 由执行线程 post 回终端线程执行 —— m_tasks / 回调都只在这里动。
void JZVirtualTerminal::finishRun(int exitCode, const string &out,
                                  const string &err, bool canceled, const string &cwd)
{
    if (m_tasks.empty())
        return;   // 已经收过尾了

    // stdout / stderr 都进"合并输出"(与旧行为一致: 回包是合并流)
    appendRunOutput(out);
    appendRunOutput(err);

    // 先把要用的东西取出来(m_tasks.clear() 之后引用就失效了)
    string text = m_tasks.front().output;   // 已含 stdout+stderr
    RunCallback finished = m_tasks.front().cb;
    const bool timedOut = (exitCode == 124 && !canceled);

    // ---- 上下文阀(见 setReplyMaxBytes): 只保留**末尾**, 开头一行显式标记 ----
    if (m_replyMaxBytes > 0 && text.size() > m_replyMaxBytes)
    {
        size_t dropped = text.size() - m_replyMaxBytes;
        text = "[输出过长: 只回末尾 " + to_string(m_replyMaxBytes) + "B(本次共 " + to_string(text.size()) +
               "B, 省略开头 " + to_string(dropped) +
               "B); 想少看就在命令里自己过滤(如 `| tail -50`), 或重定向到文件再 read_file]\n" +
               text.substr(dropped);
    }

    string reason;
    if (canceled)
        reason = "命令已被取消";
    else if (timedOut)
        reason = "命令超时, 已被强制终止";
    else if (exitCode != 0)
        reason = "退出码 " + std::to_string(exitCode);

    // 解释器是 cwd 的唯一真相: 每次收尾镜像回来(审核/展示都用它)
    if (!cwd.empty())
        m_cwd = cwd;

    m_tasks.clear();
    touch();

    if (!finished)
        return;
    finished(reason.empty(), text, reason);
}

