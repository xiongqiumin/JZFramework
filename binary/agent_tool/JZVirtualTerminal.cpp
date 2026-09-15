#include "JZVirtualTerminal.h"

#include <cstdlib>

#include "JZByteArray.h"
#include "JZDateTime.h"
#include "JZDebug.h"
#include "JZDir.h"
#include "JZFile.h"
#include "JZFileInfo.h"
#include "JZProcess.h"
#include "JZString.h"
#include "JZTimer.h"

using namespace std;

// 单次读文件上限: 工具自己还会按行裁剪, 这里只是别把几百兆的东西读进内存
static const int kMaxReadBytes = 8 * 1024 * 1024;

// ==========================================================================
// 小工具
// ==========================================================================

namespace
{

int64_t currentMs()
{
    return JZDateTime::currentMSecsSinceEpoch();
}

// 进程输出按字节原样拼进 string(JZByteArray 没有到 std::string 的隐式转换)
void appendBytes(string &dst, const JZByteArray &data)
{
    dst.append(data.data(), (size_t)data.size());
}

// 统一分隔符成 '/'
string toSlash(string path)
{
    for (char &c : path)
    {
        if (c == '\\')
            c = '/';
    }
    return path;
}

// 取父目录("a/b/c.txt" -> "a/b"; 无父目录返回空)
string dirPart(const string &path)
{
    size_t pos = path.find_last_of('/');
    if (pos == string::npos)
        return string();
    if (pos == 0)
        return "/";
    return path.substr(0, pos);
}

// 程序名归一成白名单键: 取末段、去扩展名、小写("C:/x/git.exe" -> "git")
string programKey(const string &program)
{
    string name = str_toLower(toSlash(program));
    size_t pos = name.find_last_of('/');
    if (pos != string::npos)
        name = name.substr(pos + 1);
    if (str_endsWith(name, ".exe"))
        name = name.substr(0, name.size() - 4);
    return name;
}

// 跑命令用哪个 shell。
//   POSIX   : /bin/sh
//   Windows : Git for Windows 的 Git/bin/bash.exe
//             Windows 的 PATH 里**没有** ls/cat/grep(只有 Git Bash 那份), 而 Git Bash 的
//             coreutils 与 Linux **同名** ⇒ 白名单与模型看到的命令两边一致。
// ⛔ **不退回 cmd.exe**: 找不到就返回空串, 调用方报错。理由: cmd 只有 dir/type 而且语法
//    与 Linux 完全不同, 悄悄退过去会变成"同一句话两边行为不一样", 比直接失败更难查。
#ifdef _WIN32
// 取环境变量。MSVC 把 getenv 标成 C4996, 用 _dupenv_s 免掉那个警告。
string envValue(const char *name)
{
    char *buf = nullptr;
    size_t len = 0;
    if (_dupenv_s(&buf, &len, name) != 0 || !buf)
        return string();

    string v(buf);
    free(buf);
    return v;
}
#endif

string detectShell()
{
#ifdef _WIN32
    const char *envs[] = { "ProgramFiles", "ProgramFiles(x86)", "LOCALAPPDATA" };
    const char *subs[] = { "/Git/bin/bash.exe", "/Git/bin/bash.exe", "/Programs/Git/bin/bash.exe" };

    for (int i = 0; i < 3; ++i)
    {
        string root = envValue(envs[i]);
        if (root.empty())
            continue;
        string p = toSlash(root + subs[i]);
        if (JZFileInfo(p).isFile())
            return p;
    }
    return string();        // 没装 Git for Windows
#else
    return "/bin/sh";
#endif
}

bool looksLikePath(const string &program)
{
    string p = toSlash(program);
    if (p.find('/') != string::npos)
        return true;
    return p.size() > 1 && p[1] == ':';
}

bool isBlank(const string &s)
{
    for (char c : s)
    {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
            return false;
    }
    return true;
}

string trim(const string &s)
{
    size_t b = 0;
    size_t e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\n' || s[b] == '\r'))
        b++;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\n' || s[e - 1] == '\r'))
        e--;
    return s.substr(b, e - b);
}

// 取一个片段的首 token(程序名), 去掉引号; 跳过 `NAME=value` 形式的前缀赋值。
string commandHead(const string &segment)
{
    size_t i = 0;
    while (i < segment.size())
    {
        while (i < segment.size() && (segment[i] == ' ' || segment[i] == '\t'))
            i++;
        if (i >= segment.size())
            return string();

        // 收集一个 token(带引号感知)
        string tok;
        char quote = 0;
        while (i < segment.size())
        {
            char c = segment[i];
            if (quote)
            {
                if (c == quote)
                    quote = 0;
                else
                    tok.push_back(c);
                i++;
                continue;
            }
            if (c == '\'' || c == '"')
            {
                quote = c;
                i++;
                continue;
            }
            if (c == ' ' || c == '\t')
                break;
            tok.push_back(c);
            i++;
        }

        // `FOO=bar cmd` 的前缀赋值不是程序名
        size_t eq = tok.find('=');
        if (eq != string::npos && eq > 0 && tok[0] != '/' && tok[0] != '.')
            continue;
        return tok;
    }
    return string();
}

// 把一整条命令按**顶层**运算符切开。结果: segments.size() == ops.size() + 1,
// ops[i] 是 segments[i] 与 segments[i+1] 之间的运算符("&&" / "||" / "|" / "&" / ";")。
//
// 为什么要把运算符**留下来**而不是只留切片: `&`(后台) 和 `&&`(短路) 必须分得清 ——
// 前者要拒(进程会脱离管理), 后者只是串联。旧版把两者合并了, 就拦不住 `cmd &`。
//
// 引号/转义内的运算符不算。引号不配对返回 false —— 调用方**必须拒绝**整条命令:
// 宁可拒绝, 也不能让"审不到的片段"跑起来。
bool parseTopLevel(const string &line, vector<string> &segments, vector<string> &ops)
{
    segments.clear();
    ops.clear();

    string cur;
    char quote = 0;

    for (size_t i = 0; i < line.size(); ++i)
    {
        char c = line[i];

        if (c == '\\' && i + 1 < line.size())
        {
            cur.push_back(c);
            cur.push_back(line[++i]);
            continue;
        }
        if (quote)
        {
            cur.push_back(c);
            if (c == quote)
                quote = 0;
            continue;
        }
        if (c == '\'' || c == '"')
        {
            quote = c;
            cur.push_back(c);
            continue;
        }

        string op;
        if (c == '&' || c == '|')
        {
            op = string(1, c);
            if (i + 1 < line.size() && line[i + 1] == c)   // && / || 一起吃
            {
                op += c;
                i++;
            }
        }
        else if (c == ';' || c == '\n')
            op = ";";

        if (op.empty())
        {
            cur.push_back(c);
            continue;
        }

        segments.push_back(cur);
        ops.push_back(op);
        cur.clear();
    }

    if (quote)
        return false;

    segments.push_back(cur);
    return true;
}

// 扫一遍"我们审不下去"的写法。**必须拒绝**, 不是嫌麻烦:
//   `$(...)` / `` `...` `` 把一段命令塞进参数里 —— 审核看到的首 token 是外层程序
//   (`echo $(rm -rf /)` 只看到 echo), 真正被执行的东西我们一个字都没审到。
//   `<<` heredoc / `<( )` `>( )` 进程替换同理(内容/目标不在参数位置上)。
// 单引号里的都是字面量(真 shell 也不展开), 不算命中。
bool scanUnsupported(const string &line, string &error)
{
    char quote = 0;

    for (size_t i = 0; i < line.size(); ++i)
    {
        char c = line[i];

        // 反斜杠转义(单引号里不算转义 —— shell 里单引号内一切都是字面量)
        if (c == '\\' && quote != '\'' && i + 1 < line.size())
        {
            i++;
            continue;
        }

        // 单引号内**没有任何展开** ⇒ 里面全是安全字面量, 整段跳过
        if (quote == '\'')
        {
            if (c == '\'')
                quote = 0;
            continue;
        }

        // ⚠ 双引号内 `$( )` 与反引号**照样展开** —— 所以这里只能处理引号本身的进出,
        // **不能**把后面的检查一起跳过(我第一版就是在这里 `continue` 掉了, 于是
        // `echo "a $(whoami) b"` 被放行 —— 测试直接把它抓出来了)。
        if (c == '"')
        {
            quote = quote ? 0 : '"';
            continue;
        }
        if (quote == 0 && c == '\'')
        {
            quote = '\'';
            continue;
        }

        if (c == '`')
        {
            error = "不支持反引号命令替换: 里面的命令审核不到。请拆成多条命令";
            return false;
        }
        if (c == '$' && i + 1 < line.size() && line[i + 1] == '(')
        {
            error = "不支持 $( ) 命令替换: 里面的命令审核不到。请拆成多条命令";
            return false;
        }
        if (c == '<' && i + 1 < line.size() && (line[i + 1] == '<' || line[i + 1] == '('))
        {
            error = (line[i + 1] == '<') ? "不支持 heredoc(<<): 内容不在参数位置上, 审核不到"
                                         : "不支持进程替换 <( ): 审核不到";
            return false;
        }
        if (c == '>' && i + 1 < line.size() && line[i + 1] == '(')
        {
            error = "不支持进程替换 >( ): 审核不到";
            return false;
        }
    }
    return true;
}

// 会"脱离进程管理"的命令: 一律拒绝。
// 理由不是安全洁癖 —— 它们起的进程**我们收不到输出、杀不掉**, 会话回收也收不干净,
// 机器上会慢慢攒下一堆没人管的孤儿进程。
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

// 从片段里取出重定向目标(`>` / `>>` 后面那个 token)。
// 目标带展开字符($ ~ ` *)时审不了具体是哪条路径 —— 返回 false, 调用方应当拒绝整条命令。
bool redirectTargets(const string &segment, vector<string> &out, string &error)
{
    char quote = 0;
    for (size_t i = 0; i < segment.size(); ++i)
    {
        char c = segment[i];

        if (c == '\\' && i + 1 < segment.size())
        {
            i++;
            continue;
        }
        if (quote)
        {
            if (c == quote)
                quote = 0;
            continue;
        }
        if (c == '\'' || c == '"')
        {
            quote = c;
            continue;
        }
        if (c != '>')
            continue;

        i++;
        if (i < segment.size() && segment[i] == '>')   // >>
            i++;
        while (i < segment.size() && (segment[i] == ' ' || segment[i] == '\t'))
            i++;

        string tok;
        char q2 = 0;
        while (i < segment.size())
        {
            char t = segment[i];
            if (q2)
            {
                if (t == q2)
                {
                    q2 = 0;
                    i++;
                    continue;
                }
                tok.push_back(t);
                i++;
                continue;
            }
            if (t == '\'' || t == '"')
            {
                q2 = t;
                i++;
                continue;
            }
            if (t == ' ' || t == '\t')
                break;
            tok.push_back(t);
            i++;
        }

        if (tok.empty())
        {
            error = "重定向 `>` 后面没有目标路径";
            return false;
        }
        if (tok.find('$') != string::npos || tok.find('~') != string::npos ||
            tok.find('`') != string::npos || tok.find('*') != string::npos)
        {
            error = "重定向目标带了展开字符(`$ ~ ` *`), 审不出具体写哪条路径: " + tok;
            return false;
        }
        out.push_back(tok);
    }
    return true;
}

} // namespace

// ==========================================================================
// 会话
// ==========================================================================
struct JZVirtualTerminal::Session
{
    std::string id;
    std::string cwd;                          // 规范化后的绝对目录
    std::map<std::string, std::string> env;   // 会话级环境变量覆盖
    bool running = false;                     // 有命令在途
    int64_t lastActive = 0;

    // ---- 在途命令 ----
    // proc 随会话常驻(不是每条命令新起一个), 命令跑完后同一个对象可以再 start。
    // 所有权在终端(this), 所以 Session 析构不需要管它。
    // ⚠ 回调里只按 sessionId 反查, 不捕获 Session* —— 会话被回收后指针就是悬垂的。
    JZProcess *proc = nullptr;
    JZTimer *timeoutTimer = nullptr;
    std::string output;                       // 累积输出, takeOutput 取一次清一次
    bool timedOut = false;
    RunCallback cb;                           // 在途命令的应答回调
};

// ==========================================================================
// 构造 / 策略
// ==========================================================================

JZVirtualTerminal::JZVirtualTerminal(JZObject *parent)
    : JZObject(parent)
{
    buildDefaultPrograms();
    m_shell = detectShell();
}

JZVirtualTerminal::~JZVirtualTerminal()
{
    // 先摘表: 之后任何"已结束/被杀"的回调 findSession 都返回 nullptr, 不会碰正在析构的会话。
    vector<Session *> all;
    for (auto &item : m_sessions)
        all.push_back(item.second);
    m_sessions.clear();

    for (Session *s : all)
    {
        s->running = false;
        s->cb = RunCallback();
        if (s->proc)
            s->proc->killProcess();     // proc 是终端的子对象, 由 ~JZObject 回收
        delete s;
    }
}

void JZVirtualTerminal::setZoneConfig(const ZoneConfig &zone)
{
    m_zone = zone;

    // 目录基准变了, 已有会话的 cwd 可能失效 —— 拉回工作区根(而不是让它们悬着)
    for (auto &item : m_sessions)
        item.second->cwd = m_zone.workspace;
}

const ZoneConfig &JZVirtualTerminal::zoneConfig() const
{
    return m_zone;
}

void JZVirtualTerminal::setProgramPolicy(const ProgramPolicy &policy)
{
    m_policy = policy;
}

const ProgramPolicy &JZVirtualTerminal::programPolicy() const
{
    return m_policy;
}

void JZVirtualTerminal::setShellProgram(const string &program)
{
    if (!program.empty())
        m_shell = program;
}

void JZVirtualTerminal::setTrustedPrograms(const std::vector<TrustedProgram> &programs)
{
    m_trustedPrograms.clear();
    for (const TrustedProgram &p : programs)
        m_trustedPrograms[programKey(p.name)] = p.path;
}

const map<string, string> &JZVirtualTerminal::trustedPrograms() const
{
    return m_trustedPrograms;
}

void JZVirtualTerminal::addProgram(const string &name, vector<string> exeNames, CmdCategory category,
                                   vector<string> denyFlags, vector<string> denyModules)
{
    ProgramSpec spec;
    spec.name = name;
    spec.exeNames = std::move(exeNames);
    spec.category = category;
    spec.denyFlags = std::move(denyFlags);
    spec.denyModules = std::move(denyModules);
    m_programs[programKey(name)] = spec;
}

vector<string> JZVirtualTerminal::availablePrograms() const
{
    vector<string> names;
    for (const auto &item : m_programs)
        names.push_back(item.second.name);
    return names;
}

// ==========================================================================
// 审核: 唯一入口
//
// checkRead / checkWrite / checkExec 收的都是**已经解析过的绝对路径**;
// 相对路径那一步是 resolvePath 的活。
// ==========================================================================

JZVirtualTerminal::Verdict JZVirtualTerminal::checkRead(const string &path) const
{
    Verdict v;
    Zone zone = zoneOfPath(m_zone, codeNormalizePath(path));
    if (zone == Zone::Outside)
    {
        v.decision = Decision::Deny;
        v.reason = DenyReason::PathOutside;
        v.detail = "路径越界, 只允许访问工作区与授信目录: " + path;
        return v;
    }

    v.decision = Decision::Allow;
    v.detail = string("区域: ") + zoneText(zone);
    return v;
}

JZVirtualTerminal::Verdict JZVirtualTerminal::checkWrite(const string &path) const
{
    Verdict v;
    string error;
    if (!zoneCheckWrite(m_zone, path, error))
    {
        v.decision = Decision::Deny;
        v.reason = DenyReason::WriteDenied;
        v.detail = error;
        return v;
    }

    v.decision = Decision::Allow;
    v.detail = "可写";
    return v;
}

JZVirtualTerminal::Verdict JZVirtualTerminal::checkExec(const string &program) const
{
    return checkExec(program, vector<string>());
}

JZVirtualTerminal::Verdict JZVirtualTerminal::checkExec(const string &program,
                                                       const vector<string> &args) const
{
    Verdict v;

    // ---- 显式给定路径: 必须在工作区/授信目录内, 且是可执行文件 ----
    if (looksLikePath(program))
    {
        string abs = codeNormalizePath(program);
        Zone zone = zoneOfPath(m_zone, abs);
        if (zone == Zone::Outside)
        {
            v.decision = Decision::Deny;
            v.reason = DenyReason::ProgramDenied;
            v.detail = "不允许执行该路径下的程序(不在工作区/授信目录内): " + program;
            return v;
        }

        JZFileInfo info(abs);
        if (!info.isFile())
        {
            v.decision = Decision::Deny;
            v.reason = DenyReason::NotFound;
            v.detail = "程序不存在: " + program;
            return v;
        }
#ifdef WIN32
        string suffix = str_toLower(info.suffix());
        if (suffix != "exe" && suffix != "bat" && suffix != "cmd")
        {
            v.decision = Decision::Deny;
            v.reason = DenyReason::ProgramDenied;
            v.detail = "不是可执行程序(.exe/.bat/.cmd): " + program;
            return v;
        }
#endif
        v.decision = Decision::Allow;
        v.detail = "工作区内的可执行文件";
        return v;
    }

    // ---- 按名: 查白名单 ----
    string key = programKey(program);
    auto it = m_programs.find(key);
    if (it == m_programs.end())
    {
        v.decision = Decision::Deny;
        v.reason = DenyReason::ProgramDenied;
        v.detail = "程序不在白名单里: " + program;
        return v;
    }

    const ProgramSpec &spec = it->second;

    // 分类策略(SystemAdmin 不提供开关, 永不放行)
    if (spec.category == CmdCategory::SystemAdmin)
    {
        v.decision = Decision::Deny;
        v.reason = DenyReason::EnvChange;
        v.detail = "禁止调用会改系统设置的命令: " + program;
        return v;
    }
    if (spec.category == CmdCategory::PackageManager && !m_policy.allowPackageManager)
    {
        v.decision = Decision::Deny;
        v.reason = DenyReason::EnvChange;
        v.detail = "禁止安装/升级依赖(策略未放行): " + program;
        return v;
    }
    if (spec.category == CmdCategory::Network && !m_policy.allowNetwork)
    {
        v.decision = Decision::Deny;
        v.reason = DenyReason::EnvChange;
        v.detail = "禁止从网络下载(策略未放行): " + program;
        return v;
    }

    // 参数级黑名单(前缀匹配)
    for (const string &arg : args)
    {
        for (const string &bad : spec.denyFlags)
        {
            if (str_startsWith(arg, bad))
            {
                v.decision = Decision::Deny;
                v.reason = DenyReason::CommandDenied;
                v.detail = "参数被禁止: " + program + " " + arg;
                return v;
            }
        }
    }
    // `python -m pip` 这类: 按 -m 后面跟的模块挡
    for (size_t i = 0; i + 1 < args.size(); i++)
    {
        if (args[i] != "-m")
            continue;
        for (const string &mod : spec.denyModules)
        {
            if (args[i + 1] == mod)
            {
                v.decision = Decision::Deny;
                v.reason = DenyReason::CommandDenied;
                v.detail = "模块被禁止: " + program + " -m " + mod;
                return v;
            }
        }
    }

    v.decision = Decision::Allow;
    v.detail = "白名单程序";
    return v;
}

bool JZVirtualTerminal::resolvePath(const SessionInfo &session, const string &path, bool writeAccess,
                                    string &absPath, Verdict &verdict, string &error) const
{
    string cwd = session.cwd.empty() ? m_zone.workspace : session.cwd;

    Zone zone = Zone::Outside;
    if (!zoneResolvePath(m_zone, cwd, path, absPath, zone, error))
    {
        verdict.decision = Decision::Deny;
        verdict.reason = DenyReason::PathOutside;
        verdict.detail = error;
        return false;
    }

    if (writeAccess)
    {
        string writeError;
        if (!zoneCheckWrite(m_zone, absPath, writeError))
        {
            verdict.decision = Decision::Deny;
            verdict.reason = DenyReason::WriteDenied;
            verdict.detail = writeError;
            error = writeError;
            return false;
        }
    }

    verdict.decision = Decision::Allow;
    verdict.reason = DenyReason::None;
    verdict.detail = string("区域: ") + zoneText(zone);
    error.clear();
    return true;
}

// ==========================================================================
// 会话
// ==========================================================================

JZVirtualTerminal::Session *JZVirtualTerminal::findSession(const string &id)
{
    auto it = m_sessions.find(id);
    return (it == m_sessions.end()) ? nullptr : it->second;
}

const JZVirtualTerminal::Session *JZVirtualTerminal::findSession(const string &id) const
{
    auto it = m_sessions.find(id);
    return (it == m_sessions.end()) ? nullptr : it->second;
}

void JZVirtualTerminal::touch(Session *session)
{
    if (session)
        session->lastActive = currentMs();
}

void JZVirtualTerminal::ensureReaper()
{
    if (m_reapTimer || m_recycleMs <= 0)
        return;

    m_reapTimer = new JZTimer(this);
    connect(&m_reapTimer->sigTimeout, this, [this]() { reapIdle(); });
    m_reapTimer->start(30 * 1000);   // 每 30 秒扫一次
}

void JZVirtualTerminal::reapIdle()
{
    if (m_recycleMs <= 0)
        return;

    int64_t now = currentMs();
    vector<string> dead;
    for (const auto &item : m_sessions)
    {
        Session *s = item.second;
        if (s->running)
            continue;                                   // 有命令在跑, 不算空闲
        if (now - s->lastActive < m_recycleMs)
            continue;
        dead.push_back(item.first);
    }

    for (const string &id : dead)
    {
        jzInfo().nospace() << "terminal 会话空闲超过 " << (m_recycleMs / 1000) << " 秒, 回收: " << id;
        closeSession(id);
    }
}

string JZVirtualTerminal::openSession(const string &cwd, string &error)
{
    string root = codeNormalizePath(m_zone.workspace);
    if (root.empty())
    {
        error = "工作区未配置, 不能开会话";
        return string();
    }

    string dir = cwd.empty() ? root : cwd;
    Verdict verdict;
    string abs;
    SessionInfo probe;
    probe.cwd = root;
    if (!resolvePath(probe, dir, false, abs, verdict, error))
        return string();

    JZFileInfo info(abs);
    if (!info.isDir())
    {
        error = "不是目录: " + abs;
        return string();
    }

    ensureReaper();

    Session *session = new Session();
    session->id = "t" + std::to_string(++m_idGen);
    session->cwd = abs;
    session->lastActive = currentMs();
    m_sessions[session->id] = session;

    jzInfo().nospace() << "terminal 会话开启: id=" << session->id << " cwd=" << session->cwd;
    return session->id;
}

bool JZVirtualTerminal::closeSession(const string &id)
{
    auto it = m_sessions.find(id);
    if (it == m_sessions.end())
        return false;

    Session *s = it->second;

    // 先摘表再动它: 在途命令被 kill 会同步/异步回一次 sigFinished,
    // 那时 findSession 已经查不到这个会话, 不会拿到正在释放的指针。
    m_sessions.erase(it);
    s->running = false;
    s->cb = RunCallback();

    if (s->proc)
    {
        s->proc->killProcess();
        delete s->proc;         // 主动放掉, 免得反复开会话把进程对象攒下来
    }
    delete s;
    return true;
}

vector<JZVirtualTerminal::SessionInfo> JZVirtualTerminal::sessions() const
{
    vector<SessionInfo> out;
    for (const auto &item : m_sessions)
    {
        const Session *s = item.second;
        SessionInfo info;
        info.id = s->id;
        info.cwd = s->cwd;
        info.running = s->running;
        info.lastActive = s->lastActive;
        out.push_back(info);
    }
    return out;
}

bool JZVirtualTerminal::sessionInfo(const string &id, SessionInfo &out) const
{
    const Session *s = findSession(id);
    if (!s)
        return false;

    out.id = s->id;
    out.cwd = s->cwd;
    out.running = s->running;
    out.lastActive = s->lastActive;
    return true;
}

// ==========================================================================
// 会话状态: 环境变量
//
// 只做存储与查询 —— 真正注入子进程要等 JZProcess 支持 setEnvironment()
// (现在它把 lpEnvironment 写死成 nullptr = 继承父进程)。
// ==========================================================================

bool JZVirtualTerminal::setEnv(const string &sessionId, const string &name,
                               const string &value, bool persistent)
{
    if (name.empty())
        return false;

    Session *s = findSession(sessionId);
    if (!s)
        return false;

    // Windows 环境变量名大小写不敏感: 覆盖时按不敏感匹配, 并保留原有写法
    string key = name;
    for (const auto &item : s->env)
    {
        if (str_toLower(item.first) == str_toLower(name))
        {
            key = item.first;
            break;
        }
    }

    if (persistent)
        s->env[key] = value;
    else
    {
        // 只对下一条命令生效: 记在"临时覆盖"里, run() 用完即清。
        // 目前 run 还没接, 先按持久处理并留个记号, 免得静默丢语义。
        s->env[key] = value;
        jzWarning() << "setEnv(persistent=false) 暂按持久处理(run 尚未实现)";
    }

    touch(s);
    return true;
}

bool JZVirtualTerminal::unsetEnv(const string &sessionId, const string &name)
{
    Session *s = findSession(sessionId);
    if (!s)
        return false;

    for (auto it = s->env.begin(); it != s->env.end(); ++it)
    {
        if (str_toLower(it->first) == str_toLower(name))
        {
            s->env.erase(it);
            touch(s);
            return true;
        }
    }
    return false;
}

map<string, string> JZVirtualTerminal::env(const string &sessionId) const
{
    const Session *s = findSession(sessionId);
    return s ? s->env : map<string, string>();
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
// 动作: 先把路径审核过再动手
// ==========================================================================

bool JZVirtualTerminal::checkPath(const string &sessionId, const string &path, bool writeAccess,
                                  const string &action, string &absPath, string &error)
{
    Session *s = findSession(sessionId);
    if (!s)
    {
        error = "没有这个会话: " + sessionId;
        return false;
    }
    touch(s);

    SessionInfo info;
    info.id = s->id;
    info.cwd = s->cwd;

    Verdict verdict;
    bool ok = resolvePath(info, path, writeAccess, absPath, verdict, error);
    audit(action, verdict, absPath);
    return ok;
}

bool JZVirtualTerminal::listDir(const string &sessionId, const string &path,
                                vector<string> &names, string &error)
{
    names.clear();

    string abs;
    if (!checkPath(sessionId, path.empty() ? "." : path, false, "ls", abs, error))
        return false;

    JZFileInfo info(abs);
    if (!info.exists())
    {
        error = "目录不存在: " + abs;
        return false;
    }
    if (!info.isDir())
    {
        error = "不是目录(读文件用 read): " + abs;
        return false;
    }

    vector<JZFileInfo> entries =
        JZDir(abs).entryInfoList(JZDir::Files | JZDir::Dirs | JZDir::NoDotAndDotDot, JZDir::SortName);
    for (const JZFileInfo &entry : entries)
        names.push_back(entry.isDir() ? entry.fileName() + "/" : entry.fileName());
    return true;
}

bool JZVirtualTerminal::readFile(const string &sessionId, const string &path,
                                 string &text, string &error)
{
    text.clear();

    string abs;
    if (!checkPath(sessionId, path, false, "read", abs, error))
        return false;

    JZFileInfo info(abs);
    if (!info.exists())
    {
        error = "文件不存在: " + abs;
        return false;
    }
    if (info.isDir())
    {
        error = "这是一个目录(用 ls): " + abs;
        return false;
    }
    if (info.size() > kMaxReadBytes)
    {
        error = "文件过大(>8MB), 读不了: " + abs;
        return false;
    }

    JZFile file;
    if (!file.open(abs, "rb"))
    {
        error = "打开文件失败(权限?): " + abs;
        return false;
    }
    JZByteArray data = file.readAll();
    file.close();

    text.assign(data.data(), data.size());
    return true;
}

bool JZVirtualTerminal::writeFile(const string &sessionId, const string &path,
                                  const string &text, bool append, string &error)
{
    string abs;
    if (!checkPath(sessionId, path, true, "write", abs, error))
        return false;

    JZFileInfo info(abs);
    if (info.exists() && info.isDir())
    {
        error = "目标是目录, 不能写: " + abs;
        return false;
    }

    string dir = dirPart(abs);
    if (!dir.empty())
    {
        JZDir parent;
        if (!parent.exists(dir) && !parent.mkpath(dir))
        {
            error = "创建父目录失败: " + dir;
            return false;
        }
    }

    JZFile file;
    if (!file.open(abs, append ? "ab" : "wb"))
    {
        error = "打开文件失败(权限?): " + abs;
        return false;
    }
    int written = file.write(text.data(), (int)text.size());
    file.close();

    if (written != (int)text.size())
    {
        error = "写入不完整: 期望 " + std::to_string(text.size()) + " 字节, 实际 " + std::to_string(written);
        return false;
    }
    return true;
}

bool JZVirtualTerminal::editFile(const string &sessionId, const string &path,
                                 const string &oldStr, const string &newStr,
                                 bool replaceAll, string &error)
{
    string abs;
    if (!checkPath(sessionId, path, true, "edit", abs, error))
        return false;

    // oldStr 为空 = 整文件覆盖(等价 write)
    if (oldStr.empty())
    {
        string writeError;
        bool ok = writeFile(sessionId, abs, newStr, false, writeError);
        error = writeError;
        return ok;
    }

    string content;
    if (!readFile(sessionId, abs, content, error))
        return false;

    size_t pos = 0;
    int count = 0;
    while ((pos = content.find(oldStr, pos)) != string::npos)
    {
        count++;
        pos += oldStr.size();
    }

    if (count == 0)
    {
        error = "在 " + abs + " 中找不到 old_str, 请先用 read 确认原文(空格/换行/缩进必须完全一致)";
        return false;
    }
    if (count > 1 && !replaceAll)
    {
        error = "old_str 在 " + abs + " 中匹配了 " + std::to_string(count) +
                " 处, 请补充上下文使其唯一, 或设 replace_all";
        return false;
    }

    string out;
    out.reserve(content.size());
    size_t from = 0;
    while (true)
    {
        size_t hit = content.find(oldStr, from);
        if (hit == string::npos)
        {
            out.append(content, from, string::npos);
            break;
        }
        out.append(content, from, hit - from);
        out += newStr;
        from = hit + oldStr.size();
        if (!replaceAll)
        {
            out.append(content, from, string::npos);
            break;
        }
    }

    string writeError;
    if (!writeFile(sessionId, abs, out, false, writeError))
    {
        error = writeError;
        return false;
    }
    return true;
}

bool JZVirtualTerminal::makeDir(const string &sessionId, const string &path, string &error)
{
    string abs;
    if (!checkPath(sessionId, path, true, "mkdir", abs, error))
        return false;

    JZDir dir;
    if (dir.exists(abs))
        return true;   // 已存在算成功(幂等)

    if (!dir.mkpath(abs))
    {
        error = "创建目录失败: " + abs;
        return false;
    }
    return true;
}

bool JZVirtualTerminal::removePath(const string &sessionId, const string &path,
                                   bool recursive, string &error)
{
    string abs;
    if (!checkPath(sessionId, path, true, "rm", abs, error))
        return false;

    // 兜一下: 把工作区/授信根自己删了会是灾难
    string root = codeNormalizePath(m_zone.workspace);
    if (!root.empty() && (abs == root || codeIsSubPath(root, abs)))
    {
        error = "不能删除根目录本身: " + abs;
        return false;
    }

    JZFileInfo info(abs);
    if (!info.exists())
    {
        error = "路径不存在: " + abs;
        return false;
    }

    if (info.isDir())
    {
        if (!recursive)
        {
            JZDir dir(abs);
            if (!dir.rmdir(abs))   // 只删空目录
            {
                error = "目录非空, 需要 recursive: " + abs;
                return false;
            }
            return true;
        }

        JZDir dir(abs);
        if (!dir.removeRecursively())
        {
            error = "递归删除失败(有文件被占用?): " + abs;
            return false;
        }
        return true;
    }

    if (!JZFile::remove(abs))
    {
        error = "删除失败(被占用或权限不足): " + abs;
        return false;
    }
    return true;
}

bool JZVirtualTerminal::copyPath(const string &sessionId, const string &from,
                                 const string &to, string &error)
{
    string absFrom, absTo;
    if (!checkPath(sessionId, from, false, "cp", absFrom, error))
        return false;
    // 目标可能还不存在, 所以走"写"的判定(必须在可写区域)
    if (!checkPath(sessionId, to, true, "cp", absTo, error))
        return false;

    JZFileInfo src(absFrom);
    if (!src.exists())
    {
        error = "源不存在: " + absFrom;
        return false;
    }

    if (src.isDir())
    {
        // cp -r 语义: 目标不存在就建, 存在就合并
        if (!JZDir().copyRecursively(absFrom, absTo, true, &error))
            return false;
        return true;
    }

    JZFileInfo dst(absTo);
    if (dst.isDir())
        absTo = absTo + "/" + src.fileName();

    if (!JZFile::copy(absFrom, absTo, true, &error))
        return false;
    return true;
}

bool JZVirtualTerminal::movePath(const string &sessionId, const string &from,
                                 const string &to, string &error)
{
    string absFrom, absTo;
    if (!checkPath(sessionId, from, true, "mv", absFrom, error))   // 源会消失 ⇒ 也要可写
        return false;
    if (!checkPath(sessionId, to, true, "mv", absTo, error))
        return false;

    if (!JZFileInfo::exists(absFrom))
    {
        error = "源不存在: " + absFrom;
        return false;
    }

    JZFileInfo dst(absTo);
    if (dst.isDir())
        absTo = absTo + "/" + JZFileInfo(absFrom).fileName();

    if (!JZFile::rename(absFrom, absTo, &error))
        return false;
    return true;
}

bool JZVirtualTerminal::changeDir(const string &sessionId, const string &path, string &error)
{
    Session *s = findSession(sessionId);
    if (!s)
    {
        error = "没有这个会话: " + sessionId;
        return false;
    }
    touch(s);

    SessionInfo info;
    info.cwd = s->cwd;

    Verdict verdict;
    string abs;
    if (!resolvePath(info, path.empty() ? "." : path, false, abs, verdict, error))
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
    s->cwd = abs;
    return true;
}

bool JZVirtualTerminal::printDir(const string &sessionId, string &cwd) const
{
    const Session *s = findSession(sessionId);
    if (!s)
        return false;

    cwd = s->cwd;
    return true;
}

// ==========================================================================
// 命令 —— 都交给 shell
//
// "都走 shell"是定案(2026-09-15): 管道/重定向/串联**不自己解析**, 整条命令交给
// `/bin/sh -c`(Windows 是 `cmd /c`)。理由: 目标机是 Linux, 自己再实现一遍
// ls/cat/mkdir/rm 只是重复代码 + 两套行为。
//
// 代价是审核变成**命令级**(这是这个选择的固有性质, 不是没做好):
//   - 按顶层运算符(&& || & | ; 换行)切开, 每个片段的首 token 过 checkExec 白名单;
//   - `>` / `>>` 的目标按 checkWrite 审(写只允许落在工作区);
//   - 切不干净(引号不配对)或重定向目标带展开字符 -> **拒绝整条命令**。
// ⛔ 审不到的(靠"白名单程序"那一层兜, 不是文件系统边界):
//    命令替换 $(...)/反引号内部的命令、被调用程序自己做的事(编译器/脚本能写文件)。
// ==========================================================================

bool JZVirtualTerminal::run(const string &sessionId, const string &command,
                            int timeoutMs, const RunCallback &cb, string &error)
{
    Session *s = findSession(sessionId);
    if (!s)
    {
        error = "没有这个会话: " + sessionId;
        return false;
    }
    if (command.empty())
    {
        error = "命令为空";
        return false;
    }
    if (s->running)
    {
        error = "这个会话还有命令在跑, 等它结束或先 cancel";
        return false;
    }

    // 没有 shell 就什么都跑不了 —— 明确失败, 不要静默什么都不做
    if (shellProgram().empty())
    {
        error = "没有可用的 shell: Windows 上需要 Git for Windows"
                "(没找到 Git/bin/bash.exe); 也可以用 setShellProgram() 指定";
        if (cb)
            cb(false, string(), error);
        return false;
    }

    touch(s);

    // ---- 审核: 自己解析 -> 逐段审 ----
    // 执行仍然是"整条交给 shell"(我们不自己求值管道/重定向), 但**审核必须自己解析**:
    // 不解析就只能把整串当黑盒放行, 那等于没审核。
    vector<string> segments;
    vector<string> ops;
    if (!parseTopLevel(command, segments, ops))
    {
        Verdict v;
        v.decision = Decision::Deny;
        v.reason = DenyReason::CommandDenied;
        v.detail = "命令里的引号不配对, 无法审核";
        audit("exec", v, command);
        error = v.detail;
        return false;
    }

    // 审不下去的写法($( ) / 反引号 / heredoc / 进程替换)一律拒绝
    {
        string serr;
        if (!scanUnsupported(command, serr))
        {
            Verdict v;
            v.decision = Decision::Deny;
            v.reason = DenyReason::CommandDenied;
            v.detail = serr;
            audit("exec", v, command);
            error = serr;
            return false;
        }
    }

    // 后台运行 `&`: 进程会脱离管理(拿不到输出、杀不掉), 拒绝
    for (const string &op : ops)
    {
        if (op == "&")
        {
            Verdict v;
            v.decision = Decision::Deny;
            v.reason = DenyReason::CommandDenied;
            v.detail = "不支持后台运行(`&`): 进程会脱离管理(拿不到输出、杀不掉)。"
                       "长任务请直接前台跑并设 timeout";
            audit("exec", v, command);
            error = v.detail;
            return false;
        }
    }

    // ---- 单独一条命令的特殊处理 ----
    // 每条命令都是一个新进程 ⇒ shell 内建(export/cd...)只作用在"它自己那个进程"里。
    // 不能跨命令保留的, 要么我们自己接管, 要么明确报错 —— 绝不能让模型以为"已经生效了"。
    //
    // ⚠ 只认**整条命令就一个片段**的情况。链式(如 `cd build && cmake ..`)**不拦**:
    //   交给 shell 在那一条命令内处理, 与真 shell 语义一致 —— `cd build && cmake ..`
    //   里 cmake 确实在 build/ 下跑。想让链式的 cd 也持久, 就得实现 `&&`/`||` 的短路求值
    //   (`cd a || cd b`、`cmd1 && cd x`), 那才是真的"自己实现 shell", 不值得。
    if (segments.size() == 1 && !isBlank(segments[0]))
    {
        string head = commandHead(segments[0]);

        // 1) `cd <目录>`: 我们接管, 改会话 cwd(这是唯一跨命令保留的状态)。
        //    模型要换目录就单独发一条; 夹在 `&&` 里的一次性 cd 归 shell 管。
        if (head == "cd")
        {
            string rest = segments[0];
            size_t p = rest.find("cd");
            rest = trim(rest.substr(p + 2));
            if (rest.size() >= 2 && (rest.front() == '\'' || rest.front() == '"') &&
                rest.back() == rest.front())
            {
                rest = rest.substr(1, rest.size() - 2);
            }
            if (rest.empty())
                rest = m_zone.workspace;      // 光写一个 cd = 回工作区根

            if (!changeDir(sessionId, rest, error))
            {
                if (cb)
                    cb(false, string(), error);
                return false;
            }
            if (cb)
                cb(true, s->cwd, string());   // 把新 cwd 回给调用方, 免得它猜
            return true;
        }

        // 2) 只设/清环境变量: 明确报错
        //    `export FOO=bar` 单独一条报完就没了, 但模型会以为"设过了"。
        //    夹在 `&&` 里的(`export FOO=bar && cmd`)不拦 —— 那条命令内有效, 是推荐写法。
        if (head.empty() || head == "export" || head == "unset")
        {
            Verdict v;
            v.decision = Decision::Deny;
            v.reason = DenyReason::CommandDenied;
            v.detail = "环境变量不跨命令保留(每条命令都是一个新进程): 这条命令只是在设/清变量, "
                       "下一条就没了。请写进需要它的那条命令里: `FOO=bar <命令>` "
                       "或 `export FOO=bar && <命令>`";
            audit("env", v, segments[0]);
            error = v.detail;
            return false;
        }
    }

    SessionInfo info;
    info.id = s->id;
    info.cwd = s->cwd;

    for (const string &seg : segments)
    {
        string head = commandHead(seg);
        if (head.empty())
            continue;                       // 空片段(例如结尾多的 `;`)

        // nohup / setsid / screen ... : 脱离进程管理, 拒绝(理由要说清, 别只说"不在白名单")
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

        Verdict v = checkExec(head);
        audit("exec", v, head);
        if (!v.allowed())
        {
            error = "命令被拒绝: " + head +
                    (v.detail.empty() ? string() : " (" + v.detail + ")");
            return false;
        }

        vector<string> targets;
        string terr;
        if (!redirectTargets(seg, targets, terr))
        {
            Verdict bad;
            bad.decision = Decision::Deny;
            bad.reason = DenyReason::CommandDenied;
            bad.detail = terr;
            audit("redirect", bad, seg);
            error = terr;
            return false;
        }
        for (const string &t : targets)
        {
            string abs;
            Verdict pv;
            string perr;
            if (!resolvePath(info, t, true, abs, pv, perr))
            {
                audit("redirect", pv, t);
                error = "重定向目标不可写: " + t + " (" + perr + ")";
                return false;
            }
            audit("redirect", pv, abs);
        }
    }

    // ---- 起进程 ----
    // proc 随会话建一次, 之后复用 —— 所以信号**只连一次**, 否则第 N 次 run
    // 会让回调被连 N 遍(每条命令应答 N 次)。
    string id = sessionId;
    if (!s->proc)
    {
        s->proc = new JZProcess(this);

        connect(&s->proc->sigReadyReadStandardOutput, this, [this, id]()
                {
                    Session *sess = findSession(id);
                    if (sess && sess->proc)
                        appendBytes(sess->output, sess->proc->readAllStandardOutput());
                });
        connect(&s->proc->sigFinished, this, [this, id](int exitCode, JZProcess::ExitStatus status)
                {
                    Session *sess = findSession(id);
                    if (!sess)
                        return;                     // 会话已被回收: 什么都不碰

                    if (sess->timeoutTimer)
                        sess->timeoutTimer->stop();

                    // 收尾: 管道里剩下的读完(最后一段输出 + stderr)
                    if (sess->proc)
                    {
                        appendBytes(sess->output, sess->proc->readAllStandardOutput());
                        appendBytes(sess->output, sess->proc->readAllStandardError());
                    }

                    string out = sess->output;
                    sess->output.clear();
                    sess->running = false;

                    RunCallback finished = sess->cb;
                    sess->cb = RunCallback();
                    bool timedOut = sess->timedOut;
                    sess->timedOut = false;
                    touch(sess);

                    if (!finished)
                        return;

                    if (timedOut)
                        finished(false, out, "命令超时, 已被强制终止");
                    else if (status == JZProcess::CrashExit)
                        finished(false, out, "命令异常退出");
                    else if (exitCode != 0)
                        finished(false, out, "退出码 " + std::to_string(exitCode));
                    else
                        finished(true, out, string());
                });
    }

    s->proc->setWorkingDirectory(s->cwd);
    // 合并 stdout/stderr: 分离的话模型看不到 stderr 里的编译错误
    s->proc->setProcessChannelMode(JZProcess::MergedChannels);

    s->cb = cb;
    s->timedOut = false;
    s->output.clear();

    if (timeoutMs > 0)
    {
        if (!s->timeoutTimer)
        {
            s->timeoutTimer = new JZTimer(this);
            connect(&s->timeoutTimer->sigTimeout, this, [this, id]()
                    {
                        Session *sess = findSession(id);
                        if (!sess || !sess->running || !sess->proc)
                            return;
                        sess->timedOut = true;
                        sess->proc->killProcess();   // 应答统一在 sigFinished 里做
                    });
        }
        s->timeoutTimer->start(timeoutMs);
    }

    // 整条命令交给 shell; -c 对 /bin/sh 与 Git Bash 的 bash 都适用
    bool ok = s->proc->start(shellProgram(), vector<string>{"-c", command});
    if (!ok)
    {
        if (s->timeoutTimer)
            s->timeoutTimer->stop();
        s->running = false;
        s->cb = RunCallback();
        error = "启动失败: " + command;
        if (cb)
            cb(false, string(), error);
        return false;
    }

    s->running = true;
    return true;
}

bool JZVirtualTerminal::cancel(const string &sessionId)
{
    Session *s = findSession(sessionId);
    if (!s || !s->running || !s->proc)
        return false;

    s->proc->killProcess();   // 应答走 sigFinished
    return true;
}

string JZVirtualTerminal::takeOutput(const string &sessionId)
{
    Session *s = findSession(sessionId);
    if (!s)
        return string();

    string out = s->output;
    s->output.clear();
    return out;
}

// ==========================================================================
// 默认程序白名单
//
// 定位是"副作用管控": 不让 agent 擅自装东西、下载、改系统设置。
// **不是安全边界** —— 白名单里有解释器就等于任意代码执行, 拦不住;
// 真正的边界只能是沙箱。
// ==========================================================================

void JZVirtualTerminal::buildDefaultPrograms()
{
    m_programs.clear();

    // --- 开发工具: 默认放行 ---
    addProgram("git", { "git.exe", "git" }, CmdCategory::Tool,
               { "-C", "--exec", "-c", "--upload-pack", "--upload-archive" });
    addProgram("cmake", { "cmake.exe", "cmake" }, CmdCategory::Tool);
    addProgram("tsc", { "tsc.exe", "tsc" }, CmdCategory::Tool);
    addProgram("node", { "node.exe", "node" }, CmdCategory::Tool);
    addProgram("python", { "python.exe", "python", "python3" }, CmdCategory::Tool,
               { "-c" }, { "pip" });   // python -m pip 归到"装东西"那档, 挡掉

    // --- 包管理: 默认拒绝(别装/升级依赖) ---
    addProgram("pip", { "pip.exe", "pip", "pip3.exe", "pip3" }, CmdCategory::PackageManager);
    addProgram("npm", { "npm.exe", "npm" }, CmdCategory::PackageManager);
    addProgram("npx", { "npx.exe", "npx" }, CmdCategory::PackageManager);
    addProgram("conda", { "conda.exe", "conda" }, CmdCategory::PackageManager);
    addProgram("yarn", { "yarn.exe", "yarn" }, CmdCategory::PackageManager);

    // --- 网络下载: 默认拒绝 ---
    addProgram("curl", { "curl.exe", "curl" }, CmdCategory::Network);
    addProgram("wget", { "wget.exe", "wget" }, CmdCategory::Network);

    // --- 系统管理: 永不放行 ---
    addProgram("reg", { "reg.exe", "reg" }, CmdCategory::SystemAdmin);
    addProgram("sc", { "sc.exe", "sc" }, CmdCategory::SystemAdmin);
    addProgram("net", { "net.exe", "net" }, CmdCategory::SystemAdmin);
    addProgram("setx", { "setx.exe", "setx" }, CmdCategory::SystemAdmin);
    addProgram("schtasks", { "schtasks.exe", "schtasks" }, CmdCategory::SystemAdmin);

    // --- 基础 shell / coreutils: 默认放行 ---
    // "命令都走 shell"之后, 不认这些等于什么都跑不了 —— 它们就是文件工具被删掉后的替代品。
    // 名单**两边通用**: Linux 系统自带, Windows 由 Git Bash 提供(同名, 见 detectShell())。
    // 注意这一档是**最宽**的: 白名单里有 rm/mv/chmod 就等于"能改工作区外的文件",
    // 真正的边界只能是沙箱 —— 这档只做"副作用收敛", 不是安全边界。
    static const char *kShellCmds[] = {
        // 目录 / 文件
        "ls", "cat", "mkdir", "rm", "rmdir", "cp", "mv", "touch", "ln", "chmod", "stat", "readlink",
        // 查找 / 文本处理
        "find", "grep", "egrep", "fgrep", "sed", "awk", "sort", "uniq", "cut", "tr", "head", "tail",
        "wc", "diff", "xargs", "tee", "basename", "dirname", "realpath", "seq", "printf", "nl",
        // 压缩 / 打包
        "tar", "gzip", "gunzip", "zip", "unzip",
        // shell 内建 / 环境
        // ⚠ `cd` 一定不能漏: `cd build && cmake ..` 是模型最常用的句式之一。
        "echo", "pwd", "cd", "pushd", "popd", "dirs", "env", "printenv", "export", "unset",
        "test", "[", ":", "true", "false", "source", ".", "umask", "ulimit", "kill", "wait",
        "date", "sleep", "which", "command", "type", "du", "df", "file", "less", "more",
        // 注意: 这里**故意不收** eval / exec / sh / bash —— 它们是"审不下去"的逃逸口,
        // 不收只是多一层摩擦, 不代表这是安全边界(见上面那段说明)。
    };
    for (const char *cmd : kShellCmds)
        addProgram(cmd, { cmd }, CmdCategory::Shell);
}
