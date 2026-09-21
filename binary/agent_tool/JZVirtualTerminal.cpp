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

// 一个终端同时能跑的命令条数上限。并发是为了"长任务别把别的命令全堵住",
// 不是无上限 —— 模型一口气提几十条会把机器拖死, 到顶就让它等前面的结束。
static const int kMaxConcurrentRuns = 32;
static const size_t kMaxRunOutputBytes = 8 * 1024 * 1024;   // 累积输出字节上限(防 OOM)

// ---- 编译输出错误截取 ----
// 编译/构建类的首 token: 命中才走"错误上下文"过滤(其它命令原样回传, 不误伤)。
static const char *kBuildCmds[] = {
    "make", "gmake", "cmake", "ninja", "gcc", "g++", "clang", "clang++",
    "cc", "c++", "cxx", "ld", "ar", "mingw32-make", "nmake", "msbuild",
};
static bool isBuildCommand(const string &command)
{
    // 取首个非空白 token(命令可能带 cd x && / 管道前缀, 但构建命令一般打头)
    size_t i = 0, n = command.size();
    while (i < n && isspace((unsigned char)command[i]))
        i++;
    size_t s = i;
    while (i < n && !isspace((unsigned char)command[i]))
        i++;
    if (i == s)
        return false;
    string head = command.substr(s, i - s);
    // 去掉可能的 ./ 前缀(如 ./build.sh 不算构建命令, 仅认白名单里的)
    if (head.size() >= 2 && head[0] == '.' && head[1] == '/')
        head = head.substr(2);
    for (const char *c : kBuildCmds)
    {
        if (head == c)
            return true;
    }
    return false;
}

// 一行是否算"错误/致命"(保守: 命中任一即算)
static bool isErrorLine(const string &line)
{
    string low = str_toLower(line);
    if (low.find("error:") != string::npos)   return true;   // gcc/clang/ninja
    if (low.find("fatal") != string::npos)    return true;   // fatal error
    if (low.find("undefined reference") != string::npos) return true;
    if (low.find("failed:") != string::npos)  return true;   // ninja: FAILED:
    if (low.find("ld: ") != string::npos)     return true;   // GNU ld 错误
    if (low.find("make: ***") != string::npos) return true;  // make: *** [t] Error N
    if (low.find("cmake error") != string::npos) return true; // CMake Error at ...
    return false;
}

// 编译输出过滤: 成功取尾部 kTailOnSuccess 行; 失败从开头取约 kHeadErrorCap 行错误,
// 错误块(错误行 + 其缩进续行)完整保留, 不被截断在半截。
static const int kTailOnSuccess = 20;
static const int kHeadErrorCap = 100;
string filterBuildOutput(const vector<JZVirtualTerminal::RunLogLine> &log, bool success)
{
    string out;
    if (success)
    {
        int n = (int)log.size();
        int from = (n > kTailOnSuccess) ? n - kTailOnSuccess : 0;
        for (int i = from; i < n; i++)
            out += log[i].stream + ": " + log[i].text + "\n";
        return out;
    }

    int kept = 0;
    bool inBlock = false;   // 正处于多行错误块内(错误行后的缩进续行)
    for (size_t i = 0; i < log.size(); i++)
    {
        const JZVirtualTerminal::RunLogLine &l = log[i];
        bool err = isErrorLine(l.text);
        bool keep;
        if (err)
        {
            keep = true;
            inBlock = true;
        }
        else if (inBlock)
        {
            // 错误块续行: 空行或缩进行(源片段 / `^~~` 指示符都缩进)
            if (l.text.empty() || (l.text[0] == ' ' || l.text[0] == '\t'))
                keep = true;
            else
            {
                inBlock = false;
                keep = false;
            }
        }
        else
        {
            keep = false;
        }
        // 错误前一行的上下文(通常是 make 的目标行 / 编译器调用行)
        if (!keep && i + 1 < log.size() && isErrorLine(log[i + 1].text))
            keep = true;

        if (keep)
        {
            out += l.stream + ": " + l.text + "\n";
            kept++;
            bool nextErr = (i + 1 < log.size() && isErrorLine(log[i + 1].text));
            // 到上限就停, 但不切断正在输出的错误块 / 错误前一行 / 紧邻的下一个错误
            if (kept >= kHeadErrorCap && !err && !inBlock && !nextErr)
                break;
        }
    }
    return out;
}

// 通用截断(非编译命令): 超过上限时保留头部 kGenericHead 行 + 尾部 kGenericTail 行,
// 中间省略, 避免长输出(git log / cat 大文件 / 海量 ls 等)撑爆上下文。
static const int kGenericHead = 50;
static const int kGenericTail = 50;
string truncateGeneric(const vector<JZVirtualTerminal::RunLogLine> &log)
{
    int n = (int)log.size();
    if (n <= kGenericHead + kGenericTail)
    {
        string out;
        for (int i = 0; i < n; i++)
            out += log[i].stream + ": " + log[i].text + "\n";
        return out;
    }
    string out;
    for (int i = 0; i < kGenericHead; i++)
        out += log[i].stream + ": " + log[i].text + "\n";
    out += "[... 省略 " + std::to_string(n - kGenericHead - kGenericTail) + " 行 ...]\n";
    for (int i = n - kGenericTail; i < n; i++)
        out += log[i].stream + ": " + log[i].text + "\n";
    return out;
}

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

    char buf[4096];
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
#else
string envValue(const char *name)
{
    const char *v = getenv(name);
    return v ? string(v) : string();
}
#endif

#ifndef _WIN32
// POSIX: 找一个存在的可执行文件 —— 先看几个常见系统目录(PATH 可能被裁过), 再逐段扫 PATH。
// 找到返回全路径, 找不到返回空串。
string findProgram(const string &name)
{
    static const char *kDirs[] = { "/bin", "/usr/bin", "/usr/local/bin", "/sbin", "/usr/sbin" };
    for (const char *dir : kDirs)
    {
        string p = string(dir) + "/" + name;
        if (JZFileInfo(p).isFile())
            return p;
    }

    string path = envValue("PATH");
    size_t i = 0;
    while (i < path.size())
    {
        size_t pos = path.find(':', i);
        string dir = (pos == string::npos) ? path.substr(i) : path.substr(i, pos - i);
        if (!dir.empty())
        {
            string p = toSlash(dir) + "/" + name;
            if (JZFileInfo(p).isFile())
                return p;
        }
        if (pos == string::npos)
            break;
        i = pos + 1;
    }
    return string();
}
#endif

// 探测跑命令用的 shell。
//   优先 bash: 模型写的命令基本是 bash 语法([[ ]] / 数组 / source / local); 更要紧的是
//   **没有 shebang 的脚本**, 它 ENOEXEC 之后的兜底解释器就是这里这个 shell —— 用 bash
//   能省掉一大类"语法错误(没想到我是 dash)"的坑。Windows 侧本来就是 Git Bash。
//   ⛔ 不写死 bash: Alpine/BusyBox、Yocto/Buildroot(嵌入式板子)与精简容器里常常只有
//   /bin/sh -> busybox ash, 写死会让**所有**命令都起不来。分级探测, 找不到就回退 sh。
//   两者都没有才返回空(调用方会明确报"没有可用的 shell", 不静默退回一个别的解释器)。
string detectShell()
{
#ifdef _WIN32
    // Windows 没有系统 sh: 用 Git for Windows 带的 bash。
    // ⚠ 只在 Git 的安装目录里找, **不扫 PATH** —— PATH 里叫 bash.exe 的还可能是
    //   C:\Windows\System32\bash.exe(WSL 启动器), 那会把命令送进另一个系统, 语义完全不同。
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
    string bash = findProgram("bash");
    if (!bash.empty())
        return bash;
    return findProgram("sh");
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
    // 在跑的命令全部收掉(可能多条): 先杀再删。
    // proc / timer 都是本终端的子对象, 这里显式清一次 —— 删了就会从子对象表里摘掉,
    // 与以前"单 proc"的写法一致, 不依赖 ~JZObject 的回收顺序。
    for (RunTask &t : m_tasks)
    {
        if (t.proc)
        {
            t.proc->killProcess();
            delete t.proc;
            t.proc = nullptr;
        }
        if (t.timer)
        {
            t.timer->stop();
            delete t.timer;
            t.timer = nullptr;
        }
    }
    m_tasks.clear();
}

void JZVirtualTerminal::setTerminalConfig(const JZVirtualTerminalConfig &config)
{
    m_config = config;

    // 程序映射(programPaths)是配置的一部分: 每次换配置都重新登记一遍, 故这里是动态的
    applyProgramPaths();

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

// 路径里的文件名(统一按 '/' 处理)
static string pathFileOf(const string &path)
{
    string s = toSlash(path);
    size_t pos = s.find_last_of('/');
    return pos == string::npos ? s : s.substr(pos + 1);
}

void JZVirtualTerminal::applyProgramPaths()
{
    for (const auto &kv : m_config.programPaths)
    {
        string name = str_toLower(kv.first);
        if (name.empty() || kv.second.empty())
            continue;

        // ① 补白名单: 默认表里没有的程序(如 code_runtime)按 Tool 档登记, 否则连名字都过不了审
        if (!m_programs.count(programKey(name)))
            addProgram(name, { pathFileOf(kv.second) }, CmdCategory::Tool);

        // ② 记全路径(与 setTrustedPrograms 同一张表), 供上层提示词 / 自检查询
        m_trustedPrograms[programKey(name)] = kv.second;

        // ⚠ 不把可执行文件所在目录并入授信目录: 映射只负责"短名字能过审、执行时能换成
        //   全路径", 目录授信是另一码事(想按全路径跑就自己配 zone.trustedDirs)。
        //   因此命令要写短名字 —— 写全路径走的是路径审核, 区外一律拒。
    }
}

string JZVirtualTerminal::programPath(const string &name) const
{
    auto it = m_config.programPaths.find(name);
    if (it == m_config.programPaths.end())
        it = m_config.programPaths.find(str_toLower(name));

    return it == m_config.programPaths.end() ? string() : it->second;
}

// 短程序名 -> 全路径(审核通过后原地替换):
// 映射里的程序通常不在 PATH 上(随部署走的 tsc / code_runtime), 直接把 `tsc xxx`
// 交给 shell 就是 "command not found"。做法是把命令串里"程序位置上"的那个短名
// 换成配置里的全路径, 而不是给子进程注入 PATH —— 改的是命令, 环境不动。
//
// 换哪些、换成什么, 由本终端的映射决定; 怎么改命令串是 AST 的事(JZShell::rewritePrograms:
// 按 Command::programPos/programLen 定位, 只动程序名那几个字符)。
// ⚠ 只在审核之后调 —— 换完就是全路径, 再过审核走的是路径判定(区外拒)。
string JZVirtualTerminal::expandProgramPaths(const JZShell::Line &line, const string &command) const
{
    auto resolve = [this](const JZShell::Command &cmd) -> string
    {
        string full = programPath(cmd.argv[0]);
        if (full.empty())
            return string();   // 没配映射: 原样(PATH 里自己找)

        full = toSlash(full);
        // 路径里有空格就得加引号, 否则 shell 会把它拆成两个词
        if (full.find(' ') != string::npos)
            full = "\"" + full + "\"";
        return full;
    };

    return JZShell::rewritePrograms(command, line, resolve);
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
        if (k == programKey(name))
            continue;
        auto ex = m_programs.find(k);
        if (ex != m_programs.end() && ex->second.name != name)
            jzWarning() << "addProgram: 别名 '" << exe << "' 已被程序 '" << ex->second.name
                        << "' 占用, 现被 '" << name << "' 静默覆盖";
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

    // 全部不检查(AccessLevel::Unrestricted): 程序白名单、危险命令、包管理/联网、
    // denyFlags、危险参数组合 一概不问 —— 到这里就放行。
    if (m_config.unrestricted())
    {
        v.decision = Decision::Allow;
        v.detail = "全部不检查(Unrestricted): " + program;
        return v;
    }

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
        // 已知局限: 工作区内原生程序本身可信(区域已校验), 但其**参数里写的文件路径**、
        // 以及程序**内部自己打开**的文件, 静态 AST 审核不到 —— 真正的边界是沙箱/容器。
        // 重定向(> / >>)会在后面单独遍历校验, 所以 `./app > /etc/x` 仍会被拦;
        // 但 `./app --out /etc/x`(程序内打开)无法静态感知。文档需明确这一条。
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
    // 沿父链向上追, 直到 init(ppid==1)或查不到为止; 上限 256 层兜底(正常进程树远不到,
    // 之前 32 层在极深嵌套容器里会误判成"不是自己的进程" → kill 误拒)。
    int cur = pid;
    for (int i = 0; i < 256; i++)
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

// ==========================================================================
// 安全底线: 即便 Unrestricted(全部不检查)也始终生效
//   1) 特别危险的指令(关机 / 格式化 / 往块设备写 ...)
//   2) 删除工作区外的文件(rm / rmdir 的目标不在工作区树内)
// 设计取舍: 子shell ( ) / 命令替换 $( ) 内部藏的 rm 不会被抽出(allowUnsafe 把它们
//   整段跳过), 所以底线只覆盖"顶层 / 管道"里的命令; 这正是 Unrestricted 信任档的语义
//   —— 真正的安全边界仍然是沙箱/容器, 这里只是最后一道明显自杀动作的闸。
// ==========================================================================

const char *JZVirtualTerminal::criticalDangerReason(const string &program,
                                                    const vector<string> &args) const
{
    string p = str_toLower(program);
    if (p == "shutdown" || p == "reboot" || p == "halt" || p == "poweroff" ||
        p == "init" || p == "telinit" || p == "fdisk" || p == "parted" ||
        p == "wipefs" || p == "mkswap" || p == "format" || p == "diskpart" ||
        str_startsWith(p, "mkfs") || str_startsWith(p, "mke2fs"))
    {
        return "系统/磁盘危险指令(关机/格式化/分区), 即便 Unrestricted 也禁止";
    }
    if (p == "dd")
    {
        for (const string &a : args)
        {
            if (str_startsWith(a, "of=/dev/") && a != "of=/dev/null" && a != "of=/dev/zero")
                return "不能往块设备写(dd of=/dev/...), 即便 Unrestricted 也禁止";
        }
    }
    return nullptr;
}

bool JZVirtualTerminal::isDeleteCommand(const string &programLower) const
{
    return programLower == "rm" || programLower == "rmdir" ||
           programLower == "rmtrash" || programLower == "trash" ||
           programLower == "trash-put";
}

bool JZVirtualTerminal::isWithinWorkspace(const string &absPath) const
{
    string root = codeNormalizePath(m_config.zone.workspace);
    if (root.empty())
        return true;   // 没配工作区: 无法界定"区外", 按信任档放行
    string abs = codeNormalizePath(absPath);
    return abs == root || codeIsSubPath(root, abs);
}

JZVirtualTerminal::Verdict JZVirtualTerminal::checkCriticalFloor(const JZShell::Command &cmd,
                                                                 const string &base) const
{
    Verdict v;
    v.decision = Decision::Allow;

    string head = cmd.argv.empty() ? string() : cmd.argv[0];
    if (head.empty())
        return v;

    // 1) 特别危险的指令: 永远禁止
    if (const char *why = criticalDangerReason(head, cmd.argv))
    {
        v.decision = Decision::Deny;
        v.reason = DenyReason::CommandDenied;
        v.detail = why;
        return v;
    }

    // 2) 删除工作区外的文件(rm / rmdir ...)
    string lc = str_toLower(head);
    if (isDeleteCommand(lc))
    {
        string cwd = base.empty() ? (m_cwd.empty() ? m_config.zone.workspace : m_cwd) : base;
        for (const string &a : cmd.argv)
        {
            if (a.empty() || a[0] == '-')
                continue;   // 选项(-r / -rf ...), 不是路径
            bool isAbs = (a[0] == '/') || (a.size() > 1 && a[1] == ':');
            string abs = codeNormalizePath(isAbs ? a : (cwd + "/" + a));
            // 根目录本身: 即便工作区是 "/", 也不许 rm -rf /
            string fsRoot = (abs.size() > 1 && abs[1] == ':') ? abs.substr(0, 3) : string("/");
            if (abs == fsRoot)
            {
                v.decision = Decision::Deny;
                v.reason = DenyReason::CommandDenied;
                v.detail = "Unrestricted 下仍禁止删除根目录: " + a;
                return v;
            }
            if (!isWithinWorkspace(abs))
            {
                v.decision = Decision::Deny;
                v.reason = DenyReason::CommandDenied;
                v.detail = "Unrestricted 下仍禁止删除工作区外的文件: " + a;
                return v;
            }
        }
    }
    return v;
}

bool JZVirtualTerminal::resolvePath(const string &path, bool writeAccess,
                                    string &absPath, Verdict &verdict, string &error,
                                    const string &base) const
{
    string cwd = base.empty() ? (m_cwd.empty() ? m_config.zone.workspace : m_cwd) : base;

    Zone zone = Zone::Outside;
    // 最后那个参数 = strict: "全部不检查"那档连"工作区未配置"/".. 越界"也不算失败
    if (!zoneResolvePath(m_config.zone, cwd, path, absPath, zone, error, !m_config.unrestricted()))
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

// 对外入口: 成功 = run_id, 失败 = 原因。审核/起进程在 startRun() 里 —— 中间几十处 Deny 都是
// "填 error + return false" 的老写法, 包一层比逐处改成 JZResult 干净。
JZResult<string> JZVirtualTerminal::run(const string &command, int timeoutMs, const RunCallback &cb,
                                        const std::function<void(const string &, const string &)> &onChunk,
                                        bool background,
                                        OutputFilter filter)
{
    string error;
    string runId;
    if (!startRun(command, timeoutMs, cb, error, &runId, onChunk, background, filter))
        return JZResult<string>::Error(error);
    return JZResult<string>::Ok(runId);
}

bool JZVirtualTerminal::startRun(const string &command, int timeoutMs, const RunCallback &cb, string &error,
                                 string *runId,
                                 const std::function<void(const string &, const string &)> &onChunk,
                                 bool background,
                                 OutputFilter filter)
{
    (void)background;   // 起完进程就返回(结果走 cb), 所以没有"前台阻塞"这回事了

    if (command.empty())
    {
        error = "命令为空";
        return false;
    }
    // 可以多条并发, 但有上限(见 kMaxConcurrentRuns): 到顶就等前面的结束, 或先 cancel 掉不要的。
    if ((int)m_tasks.size() >= kMaxConcurrentRuns)
    {
        error = "这个终端同时在跑的命令太多(" + std::to_string(m_tasks.size()) + "/" +
                std::to_string(kMaxConcurrentRuns) +
                "): 等其中一条结束, 或用 cancel(run_id) 停掉不要的";
        return false;
    }

    if (shellProgram().empty())
    {
        error = "没有可用的 shell: POSIX 上需要 bash 或 /bin/sh; Windows 上需要 Git for Windows"
                "(没找到 Git/bin/bash.exe); 也可以用 setShellProgram() 指定";
        if (cb)
            cb(false, string(), error);
        return false;
    }

    touch();

    // ---- 审核: 解析成语法树 -> 逐条审 ----
    // 以前是字符串扫描 + 各种特判(& 是不是后台、2>&1 是不是重定向), 现在统一由
    // JZShell 出结构, 这里只负责按结构过策略。
    // 权限等级 = Unrestricted(见 JZVirtualTerminalConfig::level): 仍解析(allowUnsafe)
    // 以跑安全底线, 但跳过白名单/区域/危险参数等全部检查; 后台(&) 也放行(信任档)。
    JZShell::Line parsed;
    string perr;
    if (m_config.unrestricted())
    {
        if (!JZShell::parse(command, parsed, perr, /*allowUnsafe=*/true))
        {
            // allowUnsafe 下解析只做结构跳过, 理论上不该失败; 万一还是失败, 保守拒绝。
            Verdict v;
            v.decision = Decision::Deny;
            v.reason = DenyReason::CommandDenied;
            v.detail = perr;
            audit("exec", v, command);
            error = perr;
            return false;
        }
    }
    else if (!JZShell::parse(command, parsed, perr))
    {
        Verdict v;
        v.decision = Decision::Deny;
        v.reason = DenyReason::CommandDenied;
        v.detail = perr;
        audit("exec", v, command);
        error = perr;
        return false;
    }
    if (parsed.background && !m_config.unrestricted())
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

            // 不限制档(Unrestricted): 跑完"安全底线"就放行, 其余检查全跳过。
            if (m_config.unrestricted())
            {
                Verdict fv = checkCriticalFloor(cmd, base);
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

    // 审核过了: 配了 programPaths 映射的短程序名, 在这里换成全路径再交给 shell。
    // (必须在审完之后 —— 换完就是路径, 再拿去审走的是路径判定, 区外一律拒)
    string execCommand = expandProgramPaths(parsed, command);

    // ---- 建运行记录 + 在途任务 ----
    // run_id 必须全局唯一: 带上终端(会话)id。否则每个会话都从 r1 开始编号,
    // 而 terminal_query 是按 run_id 全局查的(跨会话遍历注册表), 会命中别的会话的同名记录。
    string rid = (m_id.empty() ? string("r") : (m_id + "-r")) + std::to_string(++m_runIdGen);
    RunRecord rec;
    rec.runId = rid;
    rec.command = command;
    rec.status = "running";
    m_runs[rid] = rec;
    m_runOrder.push_back(rid);
    // 记录上限: 只淘汰**已结束**的旧记录。并发之后在跑的记录可能不止一条, 按老写法直接
    // 淘汰最旧的一条会把它连日志一起扔掉 —— terminal_query 就查不到还在跑的那条了。
    while (m_runOrder.size() > (size_t)kMaxRuns)
    {
        size_t victim = m_runOrder.size();
        for (size_t i = 0; i < m_runOrder.size(); i++)
        {
            auto rit = m_runs.find(m_runOrder[i]);
            if (rit != m_runs.end() && rit->second.status == "done")
            {
                victim = i;
                break;
            }
        }
        if (victim == m_runOrder.size())
            break;   // 全都还在跑: 一条也不淘汰(等它们结束自然会腾出位置)
        m_runs.erase(m_runOrder[victim]);
        m_runOrder.erase(m_runOrder.begin() + victim);
    }
    if (runId)
        *runId = rid;

    // 本次运行的输出过滤开关(默认取终端设置; 上层可按次覆盖, 见 OutputFilter)
    bool useFilter = m_filterOutput;
    if (filter == OutputFilter::On)
        useFilter = true;
    else if (filter == OutputFilter::Off)
        useFilter = false;

    // ---- 一条在途运行 = 一个进程 + 一个超时定时器 ----
    // 进程**每条命令新建**、不再复用同一个: 一个 JZProcess 只能跑一条命令, 并发下必须各起各的。
    RunTask task;
    task.runId = rid;
    task.command = command;
    task.filterOutput = useFilter;
    task.startTime = JZPlatform::getTickCountMs();
    task.cb = cb;
    task.onChunk = onChunk;
    task.proc = new JZProcess(this);
    task.proc->setWorkingDirectory(m_cwd);    // 启动那一刻的 cwd(终端级状态)
    task.proc->setProcessChannelMode(JZProcess::SeparateChannels);

    // 信号随进程各连一份, 回调只认 runId: 输出/退出都回它自己那一次运行。
    JZProcess *proc = task.proc;
    connect(&proc->sigReadyReadStandardOutput, this, [this, rid, proc]()
            {
                appendRunOutput(rid, "stdout", bytesToString(proc->readAllStandardOutput()));
            });
    connect(&proc->sigReadyReadStandardError, this, [this, rid, proc]()
            {
                appendRunOutput(rid, "stderr", bytesToString(proc->readAllStandardError()));
            });
    connect(&proc->sigFinished, this, [this, rid, proc, command](int exitCode, JZProcess::ExitStatus status)
            {
                jzDebug() << "proc finished" << rid;
                onRunFinished(rid, proc, exitCode, status, command);
            });

    if (timeoutMs > 0)
    {
        task.timer = new JZTimer(this);
        JZTimer *timer = task.timer;
        connect(&timer->sigTimeout, this, [this, rid, timer]()
                {
                    // 到点: 标记这条运行超时并强杀。杀完照样走 sigFinished, 收尾仍只有那一条路径。
                    RunTask *t = taskOf(rid);
                    if (!t || !t->proc)
                        return;
                    t->timedOut = true;
                    if (t->timer)
                    {
                        t->timer->stop();
                        t->timer->deleteLater();   // 定时器只管"到点杀一次", 用完撤掉
                        t->timer = nullptr;
                    }
                    t->proc->killProcess();
                });
        timer->start(timeoutMs);
    }

    // 先入列再 start: 启动本身就可能同步抛出输出/退出信号, 那时得能按 runId 查到这条任务。
    m_tasks.push_back(task);

    // 跑的是"换过短名"的串: 命令已按原文审过, 这里只把配了映射的程序名换成全路径
    // (运行记录/报错里仍是原文 —— 那是用户写的那条)
    bool ok = proc->start(shellProgram(), vector<string>{"-c", execCommand});
    jzDebug() << "proc start" << execCommand;
    if (!ok)
    {
        // 起不来: 撤掉这条任务与它的定时器, 记录标结束, 照常回调(别让调用方一直等)
        RunTask *t = taskOf(rid);
        if (t && t->timer)
        {
            t->timer->stop();
            delete t->timer;
            t->timer = nullptr;
        }
        eraseTask(rid);
        proc->deleteLater();
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
    int pid = proc->processId();
    if (pid > 0)
        m_ownPids.insert(pid);

    return true;
}

// 在途运行的 id(按启动先后): 上层展示/挑一条 cancel 时用。
vector<string> JZVirtualTerminal::activeRunIds() const
{
    vector<string> ids;
    ids.reserve(m_tasks.size());
    for (const RunTask &t : m_tasks)
        ids.push_back(t.runId);
    return ids;
}

bool JZVirtualTerminal::cancel(const string &runId)
{
    // 指定一条: 杀掉即可, 收尾(记结果/摘任务/回调)由它自己的 sigFinished 走。
    RunTask *t = taskOf(runId);
    if (!t || !t->proc)
        return false;
    t->proc->killProcess();
    return true;
}

int JZVirtualTerminal::cancelAll()
{
    // ⚠ 必须先把进程指针收集出来: killProcess 会(同步)触发 sigFinished -> 收尾 -> 摘掉任务,
    //   一边遍历一边删 m_tasks 会踩空。
    vector<JZProcess *> procs;
    for (RunTask &t : m_tasks)
    {
        if (t.proc)
            procs.push_back(t.proc);
    }
    for (JZProcess *p : procs)
        p->killProcess();
    return (int)procs.size();
}

string JZVirtualTerminal::takeOutput()
{
    // 并发之后"取一次输出"= 把所有在途运行的输出按启动先后拼起来, 每段各自补一次截断标记。
    // 只要某一条, 用 takeOutput(runId)。
    string out;
    for (RunTask &t : m_tasks)
    {
        if (t.output.empty() && !t.outputTruncated)
            continue;
        if (!out.empty())
            out += "\n";
        if (t.outputTruncated)
            out += "[... output truncated ...]\n";
        out += t.output;
        t.output.clear();
        t.outputTruncated = false;
    }
    return out;
}

string JZVirtualTerminal::takeOutput(const string &runId)
{
    RunTask *t = taskOf(runId);
    if (!t)
        return string();

    string out = t->output;
    if (t->outputTruncated)
        out = "\n[... output truncated ...]\n" + out;
    t->output.clear();
    t->outputTruncated = false;
    return out;
}

void JZVirtualTerminal::appendRunOutput(const string &runId, const string &stream, const string &chunk)
{
    if (chunk.empty())
        return;

    RunTask *task = taskOf(runId);
    if (task)
    {
        task->output += chunk;
        // 累积输出只保留末尾 kMaxRunOutputBytes 字节, 超了从头部裁掉, 避免长输出(编译巨量
        // 日志)把内存吃爆。截断标记在 takeOutput 时补一次(见 takeOutput)。
        if (task->output.size() > kMaxRunOutputBytes)
        {
            task->outputTruncated = true;
            task->output.erase(0, task->output.size() - kMaxRunOutputBytes);
        }
    }

    auto it = m_runs.find(runId);
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

    if (task && task->onChunk)
        task->onChunk(stream, chunk);
}

// ==========================================================================
// 在途运行的查找/收尾(全部按 runId —— 列表会随命令结束增减, 不存 RunTask*)
// ==========================================================================

JZVirtualTerminal::RunTask *JZVirtualTerminal::taskOf(const string &runId)
{
    for (RunTask &t : m_tasks)
    {
        if (t.runId == runId)
            return &t;
    }
    return nullptr;
}

const JZVirtualTerminal::RunTask *JZVirtualTerminal::taskOf(const string &runId) const
{
    for (const RunTask &t : m_tasks)
    {
        if (t.runId == runId)
            return &t;
    }
    return nullptr;
}

void JZVirtualTerminal::eraseTask(const string &runId)
{
    for (auto it = m_tasks.begin(); it != m_tasks.end(); ++it)
    {
        if (it->runId == runId)
        {
            m_tasks.erase(it);
            return;
        }
    }
}

// 一次运行的唯一收尾路径: 自然退出 / 超时被杀 / cancel 被杀 都走这里。
// 顺序: 停表 -> 取净剩余输出 -> 算回给调用方的那份(过滤) -> 记结果 -> 摘任务 -> 回调。
void JZVirtualTerminal::onRunFinished(const string &runId, JZProcess *proc, int exitCode,
                                      JZProcess::ExitStatus status, const string &command)
{
    RunTask *t = taskOf(runId);
    if (!t)
        return;   // 已经收过尾了(sigFinished 只该来一次, 保险起见)

    // 定时器: 超时那条路已经把它撤了, 这里管"命令先结束"的情况
    if (t->timer)
    {
        t->timer->stop();
        t->timer->deleteLater();
        t->timer = nullptr;
    }

    if (proc)
    {
        appendRunOutput(runId, "stdout", bytesToString(proc->readAllStandardOutput()));
        appendRunOutput(runId, "stderr", bytesToString(proc->readAllStandardError()));
    }

    // 先把这条任务上要用的东西取出来(下面 eraseTask 之后 t 就失效了)
    const bool timedOut = t->timedOut;
    const bool filterOn = t->filterOutput;
    string out = t->output;
    RunCallback finished = t->cb;

    // 输出过滤开关开(默认开): 编译命令特殊处理, 其它命令超长保留头 50 + 尾 50 行。
    //  - 编译命令: 成功取尾部 20 行, 失败取开头 ~100 行错误(错误块完整)
    // 完整日志仍可用 terminal_query 取。用逐行 RunRecord.log, 不受 output 头截断影响。
    if (filterOn)
    {
        auto fit = m_runs.find(runId);
        if (fit != m_runs.end())
        {
            if (isBuildCommand(command))
            {
                bool success = (exitCode == 0 && status == JZProcess::NormalExit && !timedOut);
                out = filterBuildOutput(fit->second.log, success);
            }
            else
            {
                out = truncateGeneric(fit->second.log);
            }
        }
    }

    auto it = m_runs.find(runId);
    if (it != m_runs.end())
    {
        RunRecord &r = it->second;
        r.status = "done";
        r.exitCode = exitCode;
        r.timedOut = timedOut;
        if (timedOut)                            r.error = "命令超时, 已被强制终止";
        else if (status == JZProcess::CrashExit)  r.error = "命令异常退出";
        else if (exitCode != 0)                   r.error = "退出码 " + std::to_string(exitCode);
    }

    eraseTask(runId);
    if (proc)
        proc->deleteLater();
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
        // `[[ ]]`: bash 的条件表达式 —— 整条链里最常写的一种。审核面只把它当"一条普通命令"
        //   (程序名是 `[[`, 后面都是参数), 结构上不需要特殊对待。默认 shell 是 bash 才有这个
        //   内建; 回退到 dash/busybox 时运行期会报 "[[ not found"(与 source 在 dash 下的
        //   情形同理: 过审靠白名单, 能不能跑由 shell 决定)。
        "test", "[", "[[", ":", "true", "false", "source", ".", "umask", "ulimit", "kill", "wait",
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
