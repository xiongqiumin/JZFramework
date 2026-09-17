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

// 单次读文件上限
static const int kMaxReadBytes = 8 * 1024 * 1024;

// 运行记录上限
static const int kMaxRuns = 200;
static const int kMaxRunLines = 4000;

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

string toSlash(string path)
{
    for (char &c : path)
    {
        if (c == '\\')
            c = '/';
    }
    return path;
}

string dirPart(const string &path)
{
    size_t pos = path.find_last_of('/');
    if (pos == string::npos)
        return string();
    if (pos == 0)
        return "/";
    return path.substr(0, pos);
}

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

#ifdef _WIN32
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
    return string();
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

string commandHead(const string &segment)
{
    size_t i = 0;
    while (i < segment.size())
    {
        while (i < segment.size() && (segment[i] == ' ' || segment[i] == '\t'))
            i++;
        if (i >= segment.size())
            return string();

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

        size_t eq = tok.find('=');
        if (eq != string::npos && eq > 0 && tok[0] != '/' && tok[0] != '.')
            continue;
        return tok;
    }
    return string();
}

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
            if (i + 1 < line.size() && line[i + 1] == c)
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

bool scanUnsupported(const string &line, string &error)
{
    char quote = 0;

    for (size_t i = 0; i < line.size(); ++i)
    {
        char c = line[i];

        if (c == '\\' && quote != '\'' && i + 1 < line.size())
        {
            i++;
            continue;
        }

        if (quote == '\'')
        {
            if (c == '\'')
                quote = 0;
            continue;
        }

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
        if (i < segment.size() && segment[i] == '>')
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
    if (m_proc)
    {
        m_proc->killProcess();     // proc 是终端的子对象, 由 ~JZObject 回收
        delete m_proc;
        m_proc = nullptr;
    }
    if (m_timeoutTimer)
    {
        delete m_timeoutTimer;
        m_timeoutTimer = nullptr;
    }
}

void JZVirtualTerminal::setZoneConfig(const ZoneConfig &zone)
{
    m_zone = zone;

    // 目录基准变了, cwd 可能失效 —— 拉回工作区根(而不是让它悬着)
    if (m_cwd.empty())
        m_cwd = m_zone.workspace;
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

bool JZVirtualTerminal::resolvePath(const string &path, bool writeAccess,
                                    string &absPath, Verdict &verdict, string &error) const
{
    string cwd = m_cwd.empty() ? m_zone.workspace : m_cwd;

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
// 终端状态
// ==========================================================================

JZVirtualTerminal::SessionInfo JZVirtualTerminal::info() const
{
    SessionInfo out;
    out.id = m_id;
    out.cwd = m_cwd;
    out.running = m_running;
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
// 动作: 先把路径审核过再动手
// ==========================================================================

bool JZVirtualTerminal::checkPath(const string &path, bool writeAccess,
                                  const string &action, string &absPath, string &error)
{
    touch();

    Verdict verdict;
    bool ok = resolvePath(path, writeAccess, absPath, verdict, error);
    audit(action, verdict, absPath);
    return ok;
}

bool JZVirtualTerminal::listDir(const string &path, vector<string> &names, string &error)
{
    names.clear();

    string abs;
    if (!checkPath(path.empty() ? "." : path, false, "ls", abs, error))
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

bool JZVirtualTerminal::readFile(const string &path, string &text, string &error)
{
    text.clear();

    string abs;
    if (!checkPath(path, false, "read", abs, error))
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

bool JZVirtualTerminal::writeFile(const string &path, const string &text, bool append, string &error)
{
    string abs;
    if (!checkPath(path, true, "write", abs, error))
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

bool JZVirtualTerminal::editFile(const string &path, const string &oldStr, const string &newStr,
                                 bool replaceAll, string &error)
{
    string abs;
    if (!checkPath(path, true, "edit", abs, error))
        return false;

    if (oldStr.empty())
    {
        string writeError;
        bool ok = writeFile(abs, newStr, false, writeError);
        error = writeError;
        return ok;
    }

    string content;
    if (!readFile(abs, content, error))
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
    if (!writeFile(abs, out, false, writeError))
    {
        error = writeError;
        return false;
    }
    return true;
}

bool JZVirtualTerminal::makeDir(const string &path, string &error)
{
    string abs;
    if (!checkPath(path, true, "mkdir", abs, error))
        return false;

    JZDir dir;
    if (dir.exists(abs))
        return true;

    if (!dir.mkpath(abs))
    {
        error = "创建目录失败: " + abs;
        return false;
    }
    return true;
}

bool JZVirtualTerminal::removePath(const string &path, bool recursive, string &error)
{
    string abs;
    if (!checkPath(path, true, "rm", abs, error))
        return false;

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
            if (!dir.rmdir(abs))
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

bool JZVirtualTerminal::copyPath(const string &from, const string &to, string &error)
{
    string absFrom, absTo;
    if (!checkPath(from, false, "cp", absFrom, error))
        return false;
    if (!checkPath(to, true, "cp", absTo, error))
        return false;

    JZFileInfo src(absFrom);
    if (!src.exists())
    {
        error = "源不存在: " + absFrom;
        return false;
    }

    if (src.isDir())
    {
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

bool JZVirtualTerminal::movePath(const string &from, const string &to, string &error)
{
    string absFrom, absTo;
    if (!checkPath(from, true, "mv", absFrom, error))
        return false;
    if (!checkPath(to, true, "mv", absTo, error))
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

bool JZVirtualTerminal::run(const string &command, int timeoutMs, const RunCallback &cb, string &error,
                            string *runId,
                            const std::function<void(const string &, const string &)> &onChunk,
                            bool background)
{
    (void)background;

    if (command.empty())
    {
        error = "命令为空";
        return false;
    }
    if (m_running)
    {
        error = "这个终端还有命令在跑, 等它结束或先 cancel";
        return false;
    }

    if (shellProgram().empty())
    {
        error = "没有可用的 shell: Windows 上需要 Git for Windows"
                "(没找到 Git/bin/bash.exe); 也可以用 setShellProgram() 指定";
        if (cb)
            cb(false, string(), error);
        return false;
    }

    touch();

    // ---- 审核: 自己解析 -> 逐段审 ----
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

    // 单独一条命令的特殊处理
    if (segments.size() == 1 && !isBlank(segments[0]))
    {
        string head = commandHead(segments[0]);

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
                rest = m_zone.workspace;

            if (!changeDir(rest, error))
            {
                if (cb)
                    cb(false, string(), error);
                return false;
            }
            if (cb)
                cb(true, m_cwd, string());
            return true;
        }

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

    for (const string &seg : segments)
    {
        string head = commandHead(seg);
        if (head.empty())
            continue;

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
            if (!resolvePath(t, true, abs, pv, perr))
            {
                audit("redirect", pv, t);
                error = "重定向目标不可写: " + t + " (" + perr + ")";
                return false;
            }
            audit("redirect", pv, abs);
        }
    }

    // ---- 起进程 + 建运行记录 ----
    // proc 随终端建一次, 之后复用 —— 所以信号只连一次。
    string rid = "r" + std::to_string(++m_runIdGen);
    RunRecord rec;
    rec.runId = rid;
    rec.command = command;
    rec.status = "running";
    m_runs[rid] = rec;
    m_runOrder.push_back(rid);
    while (m_runOrder.size() > (size_t)kMaxRuns)
    {
        m_runs.erase(m_runOrder.front());
        m_runOrder.erase(m_runOrder.begin());
    }
    if (runId)
        *runId = rid;

    if (!m_proc)
    {
        m_proc = new JZProcess(this);

        connect(&m_proc->sigReadyReadStandardOutput, this, [this]()
                {
                    if (m_proc)
                        appendRunOutput("stdout", bytesToString(m_proc->readAllStandardOutput()));
                });
        connect(&m_proc->sigReadyReadStandardError, this, [this]()
                {
                    if (m_proc)
                        appendRunOutput("stderr", bytesToString(m_proc->readAllStandardError()));
                });
        connect(&m_proc->sigFinished, this, [this](int exitCode, JZProcess::ExitStatus status)
                {
                    if (!m_proc)
                        return;

                    if (m_timeoutTimer)
                        m_timeoutTimer->stop();

                    appendRunOutput("stdout", bytesToString(m_proc->readAllStandardOutput()));
                    appendRunOutput("stderr", bytesToString(m_proc->readAllStandardError()));

                    auto it = m_runs.find(m_runId);
                    if (it != m_runs.end())
                    {
                        RunRecord &r = it->second;
                        r.status = "done";
                        r.exitCode = exitCode;
                        r.timedOut = m_timedOut;
                        if (m_timedOut)                r.error = "命令超时, 已被强制终止";
                        else if (status == JZProcess::CrashExit) r.error = "命令异常退出";
                        else if (exitCode != 0)           r.error = "退出码 " + std::to_string(exitCode);
                    }

                    string out = m_output;
                    m_output.clear();
                    m_running = false;

                    RunCallback finished = m_cb;
                    m_cb = RunCallback();
                    bool timedOut = m_timedOut;
                    m_timedOut = false;
                    touch();

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

    m_proc->setWorkingDirectory(m_cwd);
    m_proc->setProcessChannelMode(JZProcess::SeparateChannels);

    m_cb = cb;
    m_timedOut = false;
    m_output.clear();
    m_runId = rid;
    m_onChunk = onChunk;

    if (timeoutMs > 0)
    {
        if (!m_timeoutTimer)
        {
            m_timeoutTimer = new JZTimer(this);
            connect(&m_timeoutTimer->sigTimeout, this, [this]()
                    {
                        if (!m_running || !m_proc)
                            return;
                        m_timedOut = true;
                        m_proc->killProcess();
                    });
        }
        m_timeoutTimer->start(timeoutMs);
    }

    bool ok = m_proc->start(shellProgram(), vector<string>{"-c", command});
    if (!ok)
    {
        if (m_timeoutTimer)
            m_timeoutTimer->stop();
        m_running = false;
        m_cb = RunCallback();
        m_onChunk = nullptr;
        auto it = m_runs.find(rid);
        if (it != m_runs.end())
        {
            it->second.status = "done";
            it->second.error = "启动失败: " + command;
        }
        error = "启动失败: " + command;
        if (cb)
            cb(false, string(), error);
        return false;
    }

    m_running = true;
    return true;
}

bool JZVirtualTerminal::cancel()
{
    if (!m_running || !m_proc)
        return false;

    m_proc->killProcess();
    return true;
}

string JZVirtualTerminal::takeOutput()
{
    string out = m_output;
    m_output.clear();
    return out;
}

void JZVirtualTerminal::appendRunOutput(const string &stream, const string &chunk)
{
    if (chunk.empty())
        return;

    m_output += chunk;

    auto it = m_runs.find(m_runId);
    if (it != m_runs.end())
    {
        RunRecord &rec = it->second;
        size_t pos = 0;
        while (true)
        {
            size_t nl = chunk.find('\n', pos);
            string line = (nl == string::npos) ? chunk.substr(pos) : chunk.substr(pos, nl - pos);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (!line.empty())
                rec.log.push_back({stream, line});
            if (nl == string::npos)
                break;
            pos = nl + 1;
        }
        while (rec.log.size() > (size_t)kMaxRunLines)
            rec.log.erase(rec.log.begin());
    }

    if (m_onChunk)
        m_onChunk(stream, chunk);
}

bool JZVirtualTerminal::runSnapshot(const string &runId, RunRecordInfo &out) const
{
    auto it = m_runs.find(runId);
    if (it == m_runs.end())
        return false;
    const RunRecord &r = it->second;
    out.runId = r.runId;
    out.command = r.command;
    out.status = r.status;
    out.exitCode = r.exitCode;
    out.timedOut = r.timedOut;
    out.error = r.error;
    out.log = r.log;
    return true;
}

vector<string> JZVirtualTerminal::runIds() const
{
    return m_runOrder;
}

// ==========================================================================
// 默认程序白名单
// ==========================================================================

void JZVirtualTerminal::buildDefaultPrograms()
{
    m_programs.clear();

    addProgram("git", { "git.exe", "git" }, CmdCategory::Tool,
               { "-C", "--exec", "-c", "--upload-pack", "--upload-archive" });
    addProgram("cmake", { "cmake.exe", "cmake" }, CmdCategory::Tool);
    addProgram("tsc", { "tsc.exe", "tsc" }, CmdCategory::Tool);
    addProgram("node", { "node.exe", "node" }, CmdCategory::Tool);
    addProgram("python", { "python.exe", "python", "python3" }, CmdCategory::Tool,
               { "-c" }, { "pip" });

    addProgram("pip", { "pip.exe", "pip", "pip3.exe", "pip3" }, CmdCategory::PackageManager);
    addProgram("npm", { "npm.exe", "npm" }, CmdCategory::PackageManager);
    addProgram("npx", { "npx.exe", "npx" }, CmdCategory::PackageManager);
    addProgram("conda", { "conda.exe", "conda" }, CmdCategory::PackageManager);
    addProgram("yarn", { "yarn.exe", "yarn" }, CmdCategory::PackageManager);

    addProgram("curl", { "curl.exe", "curl" }, CmdCategory::Network);
    addProgram("wget", { "wget.exe", "wget" }, CmdCategory::Network);

    addProgram("reg", { "reg.exe", "reg" }, CmdCategory::SystemAdmin);
    addProgram("sc", { "sc.exe", "sc" }, CmdCategory::SystemAdmin);
    addProgram("net", { "net.exe", "net" }, CmdCategory::SystemAdmin);
    addProgram("setx", { "setx.exe", "setx" }, CmdCategory::SystemAdmin);
    addProgram("schtasks", { "schtasks.exe", "schtasks" }, CmdCategory::SystemAdmin);

    static const char *kShellCmds[] = {
        "ls", "cat", "mkdir", "rm", "rmdir", "cp", "mv", "touch", "ln", "chmod", "stat", "readlink",
        "find", "grep", "egrep", "fgrep", "sed", "awk", "sort", "uniq", "cut", "tr", "head", "tail",
        "wc", "diff", "xargs", "tee", "basename", "dirname", "realpath", "seq", "printf", "nl",
        "echo", "pwd", "cd", "pushd", "popd", "dirs", "env", "printenv", "export", "unset",
        "test", "[", ":", "true", "false", "source", ".", "umask", "ulimit", "kill", "wait",
        "date", "sleep", "which", "command", "type", "du", "df", "file", "less", "more",
    };
    for (const char *cmd : kShellCmds)
        addProgram(cmd, { cmd }, CmdCategory::Shell);
}
