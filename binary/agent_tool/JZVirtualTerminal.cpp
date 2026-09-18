#include "JZVirtualTerminal.h"

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
#include "JZDir.h"
#include "JZFile.h"
#include "JZFileInfo.h"
#include "JZProcess.h"
#include "JZShellParser.h"
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

// 取父进程号(kill 归属判定要用: 得知道这个 pid 是不是本终端起的)。失败返回 -1。
int parentPidOf(int pid)
{
    if (pid <= 0)
        return -1;

#ifdef _WIN32
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return -1;

    PROCESSENTRY32W pe;
    memset(&pe, 0, sizeof(pe));
    pe.dwSize = sizeof(pe);
    int ppid = -1;
    if (Process32FirstW(snap, &pe))
    {
        do
        {
            if ((int)pe.th32ProcessID == pid)
            {
                ppid = (int)pe.th32ParentProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return ppid;
#else
    // /proc/<pid>/stat 形如 "1234 (bash) S 1200 ..." —— comm 里可能有空格和括号,
    // 所以从最后一个 ')' 往后读, 跳过 state 就是 ppid
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);

    FILE *f = fopen(path, "r");
    if (!f)
        return -1;

    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);

    string s(buf);
    size_t rb = s.rfind(')');
    if (rb == string::npos)
        return -1;

    char state = 0;
    int ppid = -1;
    if (sscanf(s.c_str() + rb + 1, " %c %d", &state, &ppid) != 2)
        return -1;
    return ppid;
#endif
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

void JZVirtualTerminal::setTerminalConfig(const JZVirtualTerminalConfig &config)
{
    m_config = config;

    // 目录基准变了, cwd 可能失效 —— 拉回工作区根(而不是让它悬着)
    if (m_cwd.empty())
        m_cwd = m_config.zone.workspace;
}

const JZVirtualTerminalConfig &JZVirtualTerminal::terminalConfig() const
{
    return m_config;
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

    // 可执行文件别名也要能查到: 否则 `python3 x.py` 会因为表里只有 "python" 被当成
    // 陌生程序拒掉 —— exeNames 本来就是"这个程序还可能叫什么"
    for (const string &exe : spec.exeNames)
    {
        string k = programKey(exe);
        if (k != programKey(name))
            m_programs[k] = spec;
    }
}

vector<string> JZVirtualTerminal::availablePrograms() const
{
    vector<string> names;
    for (const auto &item : m_programs)
        names.push_back(item.second.name);

    // 别名(python / python3 指向同一条)会重复出现, 去重后再给出去
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

// ==========================================================================
// 审核: 唯一入口
// ==========================================================================

JZVirtualTerminal::Verdict JZVirtualTerminal::checkRead(const string &path) const
{
    Verdict v;
    string abs = codeNormalizePath(path);
    string error;
    if (!zoneCheckAccess(m_config, abs, FileAccess::Read, error))
    {
        v.decision = Decision::Deny;
        v.reason = DenyReason::AccessDenied;
        v.detail = error;
        return v;
    }

    v.decision = Decision::Allow;
    v.detail = string("区域: ") + zoneText(zoneOfPath(m_config.zone, abs));
    return v;
}

JZVirtualTerminal::Verdict JZVirtualTerminal::checkWrite(const string &path) const
{
    Verdict v;
    string abs = codeNormalizePath(path);
    string error;
    if (!zoneCheckAccess(m_config, abs, FileAccess::Write, error))
    {
        v.decision = Decision::Deny;
        v.reason = DenyReason::WriteDenied;
        v.detail = error;
        return v;
    }

    v.decision = Decision::Allow;
    v.detail = string("可写, 区域: ") + zoneText(zoneOfPath(m_config.zone, abs));
    return v;
}

JZVirtualTerminal::Verdict JZVirtualTerminal::checkExec(const string &program) const
{
    return checkExec(program, vector<string>());
}

JZVirtualTerminal::Verdict JZVirtualTerminal::checkExec(const string &program,
                                                       const vector<string> &args,
                                                       const string &base) const
{
    Verdict v;
    string cwd = base.empty() ? (m_cwd.empty() ? m_config.zone.workspace : m_cwd) : base;

    if (looksLikePath(program))
    {
        // 相对路径(./hello、build/app)必须按基准目录解析, 否则会被当成区外程序拒掉
        string p = toSlash(program);
        bool isAbs = (!p.empty() && p[0] == '/') || (p.size() > 1 && p[1] == ':');
        string abs = codeNormalizePath(isAbs ? p : (cwd + "/" + p));
        // 工作区/授信目录里的程序默认可执行(编译产物 ./app、SDK 里的工具);
        // 区外程序不许直接按路径跑 —— 要走白名单(白名单里的 g++ 之类不受此限)
        string execError;
        if (!zoneCheckAccess(m_config, abs, FileAccess::Exec, execError))
        {
            v.decision = Decision::Deny;
            v.reason = DenyReason::ProgramDenied;
            v.detail = execError;
            return v;
        }

        // 不在审核阶段检查目标是否存在: 编译产物(./app)往往由同一条链路里的前序命令
        // 生成(如 `g++ -o hello hello.cpp && ./hello`), 静态审核时它还不存在, 预先判
        // "不存在"会把合理的 build+run 整条拦掉。路径合法(区域允许执行)即放行,
        // 目标不存在时由 OS 在执行期报 "No such file" —— 与普通 shell 行为一致, 也不削弱
        // 安全(真正的边界是上面的 zoneCheckAccess: 区外程序一律拒绝)。
        // 仅对"已存在但后缀不对"的可执行(Windows)做后缀拦截; 缺失文件跳过(运行时再说)。
#ifdef WIN32
        JZFileInfo info(abs);
        if (info.isFile())
        {
            string suffix = str_toLower(info.suffix());
            if (suffix != "exe" && suffix != "bat" && suffix != "cmd")
            {
                v.decision = Decision::Deny;
                v.reason = DenyReason::ProgramDenied;
                v.detail = "不是可执行程序(.exe/.bat/.cmd): " + program;
                return v;
            }
        }
#endif
        v.decision = Decision::Allow;
        v.detail = "工作区内的可执行文件(执行期若不存在由 OS 报错)";
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
    if (spec.category == CmdCategory::Dangerous)
    {
        v.decision = Decision::Deny;
        v.reason = DenyReason::CommandDenied;
        v.detail = "危险命令, 不放行: " + program;
        return v;
    }
    if (spec.category == CmdCategory::PackageManager && !m_config.allowPackageManager)
    {
        v.decision = Decision::Deny;
        v.reason = DenyReason::EnvChange;
        v.detail = "禁止安装/升级依赖(策略未放行): " + program;
        return v;
    }
    if (spec.category == CmdCategory::Network && !m_config.allowNetwork)
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
    if (const char *why = dangerousArgReason(program, args, cwd))
    {
        v.decision = Decision::Deny;
        v.reason = DenyReason::CommandDenied;
        v.detail = string("危险用法, 不放行: ") + program + " —— " + why;
        return v;
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

const char *JZVirtualTerminal::dangerousArgReason(const string &program,
                                                  const vector<string> &args,
                                                  const string &base) const
{
    string prog = str_toLower(program);

    // kill: 默认放行, 但只许杀自己启动的进程
    if (prog == "kill" || prog == "killall" || prog == "pkill")
    {
        // killall / pkill 按名字杀, 判不出归属(会连带杀别人的), 一律不干
        if (prog != "kill")
            return "按名字杀进程无法判定归属, 请改用 kill <pid>(只能杀本终端启动的)";

        for (const string &a : args)
        {
            if (a.empty() || a[0] == '-')
                continue;   // -9 / -TERM 等信号
            if (a[0] == '%')
                return "不支持作业号(如 %1), 请直接给 pid";

            int pid = atoi(a.c_str());
            if (pid > 0 && !isOwnProcess(pid))
                return "只能 kill 本终端自己启动的进程(给了别人的 pid)";
        }
        return nullptr;   // kill -l 之类没有 pid 的用法照常放行
    }

    if (prog == "rm")
    {
        // 删也是"写": 按写权限判(工作区 + 系统临时目录 + 标了可写的授信目录),
        // 别处一律不让删。工作区里的 build/、out/、junk/ 照删 —— 那是它自己的产物。
        bool recursive = false;
        for (const string &a : args)
        {
            if (a == "-r" || a == "-rf" || a == "-fr" || a == "-R" || a == "-Rf" || a == "-fR")
                recursive = true;
        }

        for (const string &a : args)
        {
            if (a.empty() || a[0] == '-')
                continue;

            bool isAbs = (a[0] == '/') || (a.size() > 1 && a[1] == ':');
            string cwd = base.empty() ? (m_cwd.empty() ? m_config.zone.workspace : m_cwd) : base;
            string abs = codeNormalizePath(isAbs ? a : (cwd + "/" + a));
            string werr;
            if (!zoneCheckAccess(m_config, abs, FileAccess::Write, werr))
                return recursive ? "不能递归删除没有写权限的目录"
                                 : "不能删除没有写权限的文件";
        }
        return nullptr;
    }

    if (prog == "dd")
    {
        for (const string &a : args)
        {
            if (str_startsWith(a, "of=/dev/") && a != "of=/dev/null" && a != "of=/dev/zero")
                return "不能往块设备写";
        }
    }

    return nullptr;
}

bool JZVirtualTerminal::isOwnProcess(int pid) const
{
    if (pid <= 0)
        return false;
    if (m_ownPids.count(pid))
        return true;

    // 子进程也算自己起的: make 拉的 gcc、sh 拉的子命令, 它们的祖先链上有我们记的根 pid。
    // 往上最多追 32 层, 到 init / 查不到就停。
    int cur = pid;
    for (int i = 0; i < 32; i++)
    {
        int ppid = parentPidOf(cur);
        if (ppid <= 0 || ppid == 1)
            return false;
        if (m_ownPids.count(ppid))
            return true;
        cur = ppid;
    }
    return false;
}

bool JZVirtualTerminal::resolvePath(const string &path, bool writeAccess,
                                    string &absPath, Verdict &verdict, string &error,
                                    const string &base) const
{
    string cwd = base.empty() ? (m_cwd.empty() ? m_config.zone.workspace : m_cwd) : base;

    Zone zone = Zone::Outside;
    if (!zoneResolvePath(m_config.zone, cwd, path, absPath, zone, error))
    {
        verdict.decision = Decision::Deny;
        verdict.reason = DenyReason::PathOutside;
        verdict.detail = error;
        return false;
    }

    // 读按 Read 位判(区外默认只读, 所以读 /etc/hosts 能过), 写按 Write 位判(默认只有工作区)
    int need = writeAccess ? (FileAccess::Read | FileAccess::Write) : FileAccess::Read;
    string accessError;
    if (!zoneCheckAccess(m_config, absPath, need, accessError))
    {
        verdict.decision = Decision::Deny;
        verdict.reason = writeAccess ? DenyReason::WriteDenied : DenyReason::AccessDenied;
        verdict.detail = accessError;
        error = accessError;
        return false;
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
                                  const string &action, string &absPath, string &error,
                                  const string &base)
{
    touch();

    Verdict verdict;
    bool ok = resolvePath(path, writeAccess, absPath, verdict, error, base);
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

    string root = codeNormalizePath(m_config.zone.workspace);
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

    // ---- 审核: 解析成语法树 -> 逐条审 ----
    // 以前是字符串扫描 + 各种特判(& 是不是后台、2>&1 是不是重定向), 现在统一由
    // JZShell 出结构, 这里只负责按结构过策略。
    JZShell::Line parsed;
    string perr;
    if (!JZShell::parse(command, parsed, perr))
    {
        Verdict v;
        v.decision = Decision::Deny;
        v.reason = DenyReason::CommandDenied;
        v.detail = perr;
        audit("exec", v, command);
        error = perr;
        return false;
    }
    if (parsed.background)
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

    // 单独一条命令的特殊处理
    if (parsed.pipelines.size() == 1 && parsed.pipelines[0].commands.size() == 1)
    {
        const JZShell::Command &cmd = parsed.pipelines[0].commands[0];
        string head = cmd.argv.empty() ? string() : cmd.argv[0];

        if (head == "cd")
        {
            // 每条命令都是一个新的 sh: cd 只在"这一条命令"里有效, 单独发一条什么也留不下。
            // 与其让它悄悄失效(模型还以为自己在那个目录), 不如直接点破正确写法。
            Verdict v;
            v.decision = Decision::Deny;
            v.reason = DenyReason::CommandDenied;
            v.detail = "单独的 cd 没有意义: 每条命令都是一个新的 shell, cd 影响不到后面的命令。"
                       "要在某个目录里执行就写 `cd <目录> && <命令>`(只对这一条生效); "
                       "要一直待在某处, 用工具自带的 -C 参数(如 make -C build)";
            audit("cd", v, head);
            error = v.detail;
            return false;
        }

        if (head.empty() || head == "export" || head == "unset")
        {
            Verdict v;
            v.decision = Decision::Deny;
            v.reason = DenyReason::CommandDenied;
            v.detail = "环境变量不跨命令保留(每条命令都是一个新进程): 这条命令只是在设/清变量, "
                       "下一条就没了。请写进需要它的那条命令里: `FOO=bar <命令>` "
                       "或 `export FOO=bar && <命令>`";
            audit("env", v, head);
            error = v.detail;
            return false;
        }
    }

    // 审核基准: 从工作区根出发。遇到 `cd X && ...` 要跟着走进 X —— 因为 sh 是真的会
    // chdir 过去的, 后面那些相对路径是相对 X 的; 静态检查不跟着走, 就会出现"审的是
    // 工作区里的 f、实际写的是 X/f", 写保护直接被绕过去。
    string base = m_cwd.empty() ? m_config.zone.workspace : m_cwd;

    for (size_t pi = 0; pi < parsed.pipelines.size(); pi++)
    {
        const JZShell::Pipeline &pl = parsed.pipelines[pi];

        // 单条(非管道)的 cd: 只改这一条命令的基准, 不改终端状态。
        // 管道里的 cd 在子 shell 里跑, 影响不到后面的段, 所以不算数。
        if (pl.commands.size() == 1 && !pl.commands[0].argv.empty()
            && pl.commands[0].argv[0] == "cd")
        {
            const JZShell::Command &cd = pl.commands[0];
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

        for (const JZShell::Command &cmd : pl.commands)
        {
            string head = cmd.argv.empty() ? string() : cmd.argv[0];
            if (head.empty())
                continue;   // 只有 VAR=val 前缀的一段(如 `FOO=bar && cmd`), 没有程序可调

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
            for (const JZShell::Redirect &r : cmd.redirects)
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

    // ---- 起进程 + 建运行记录 ----
    // proc 随终端建一次, 之后复用 —— 所以信号只连一次。
    // run_id 必须全局唯一: 带上终端(会话)id。否则每个会话都从 r1 开始编号,
    // 而 terminal_query 是按 run_id 全局查的(跨会话遍历注册表), 会命中别的会话的同名记录。
    string rid = (m_id.empty() ? string("r") : (m_id + "-r")) + std::to_string(++m_runIdGen);
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

    // 记下根 pid: kill 的归属判定靠它(只许杀自己启动的进程)
    int pid = m_proc->processId();
    if (pid > 0)
        m_ownPids.insert(pid);

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

    // C/C++ 构建链: 没有编译器就没法在这上面编译工程
    addProgram("gcc", { "gcc.exe", "gcc" }, CmdCategory::Tool);
    addProgram("g++", { "g++.exe", "g++" }, CmdCategory::Tool);
    addProgram("cc", { "cc" }, CmdCategory::Tool);
    addProgram("c++", { "c++" }, CmdCategory::Tool);
    addProgram("clang", { "clang.exe", "clang" }, CmdCategory::Tool);
    addProgram("clang++", { "clang++.exe", "clang++" }, CmdCategory::Tool);
    addProgram("make", { "make.exe", "make" }, CmdCategory::Tool);
    addProgram("ninja", { "ninja.exe", "ninja" }, CmdCategory::Tool);
    addProgram("ar", { "ar.exe", "ar" }, CmdCategory::Tool);
    addProgram("cmake", { "cmake.exe", "cmake" }, CmdCategory::Tool);
    addProgram("ctest", { "ctest.exe", "ctest" }, CmdCategory::Tool);
    addProgram("pkg-config", { "pkg-config.exe", "pkg-config" }, CmdCategory::Tool);
    addProgram("ccache", { "ccache.exe", "ccache" }, CmdCategory::Tool);
    // 查链接/符号问题用得到(nm/objdump/readelf/addr2line)
    addProgram("ld", { "ld" }, CmdCategory::Tool);
    addProgram("nm", { "nm" }, CmdCategory::Tool);
    addProgram("objdump", { "objdump" }, CmdCategory::Tool);
    addProgram("readelf", { "readelf" }, CmdCategory::Tool);
    addProgram("addr2line", { "addr2line" }, CmdCategory::Tool);
    addProgram("strip", { "strip" }, CmdCategory::Tool);
    addProgram("size", { "size" }, CmdCategory::Tool);
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

    // 危险/破坏性命令: 一律拒绝, 不提供开关。
    // 它们本来也不在白名单里(会报"不在白名单"), 单列一档是为了给一句人看得懂的话 ——
    // 模型看到"危险命令, 不放行"才知道换路子, 而不是反复重试。
    static const char *kDangerous[] = {
        // 动磁盘/文件系统
        "dd", "mkfs", "mke2fs", "mkfs.ext4", "fdisk", "sfdisk", "parted", "fsck", "mkswap",
        "swapon", "swapoff", "mount", "umount",
        // 开关机/系统服务
        "shutdown", "reboot", "halt", "poweroff", "init", "telinit", "systemctl", "service",
        // 提权与账号
        "sudo", "su", "passwd", "chpasswd", "useradd", "userdel", "usermod", "groupadd",
        "groupdel", "chroot",
        // 权限归属
        "chown", "chgrp", "setfacl", "chattr",
        // 内核/防火墙/计划任务
        "modprobe", "insmod", "rmmod", "iptables", "ufw", "sysctl", "crontab",
        // 杀别人的进程(kill 自己起的进程用 kill)
        "killall", "pkill",
    };
    for (const char *cmd : kDangerous)
        addProgram(cmd, { cmd }, CmdCategory::Dangerous);

    static const char *kShellCmds[] = {
        "ls", "cat", "mkdir", "rm", "rmdir", "cp", "mv", "touch", "ln", "chmod", "stat", "readlink",
        "find", "grep", "egrep", "fgrep", "sed", "awk", "sort", "uniq", "cut", "tr", "head", "tail",
        "wc", "diff", "xargs", "tee", "basename", "dirname", "realpath", "seq", "printf", "nl",
        "echo", "pwd", "cd", "pushd", "popd", "dirs", "env", "printenv", "export", "unset",
        "test", "[", ":", "true", "false", "source", ".", "umask", "ulimit", "kill", "wait",
        "date", "sleep", "which", "command", "type", "du", "df", "file", "less", "more",
        // 打包/压缩(拉依赖、解源码包常用)
        "tar", "zip", "unzip", "gzip", "gunzip", "bzip2", "bunzip2", "xz", "unxz", "zstd",
        // 其它常用
        "patch", "mktemp", "install", "cmp", "split", "paste", "join", "od", "xxd", "truncate",
        "md5sum", "sha1sum", "sha256sum", "cksum", "timeout", "time", "nproc", "uname",
        "hostname", "id", "whoami", "ps", "free",
    };
    for (const char *cmd : kShellCmds)
        addProgram(cmd, { cmd }, CmdCategory::Shell);
}
