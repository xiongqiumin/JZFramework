#include "JZShellPolicy.h"

#include <algorithm>

#include "JZCodeTools.h"   // codeNormalizePath / codeIsSubPath(路径规范化与区域判定)
#include "JZDebug.h"       // jzWarning
#include "JZFileInfo.h"    // Windows 上"已存在但后缀不对"的拦截
#include "JZString.h"      // str_toLower / str_startsWith / str_endsWith / toSlash

using namespace std;

namespace
{
// 反斜杠统一成正斜杠(判定用的路径只认 /)
string toSlash(string path)
{
    for (char &c : path)
    {
        if (c == '\\')
            c = '/';
    }
    return path;
}

// 程序名归一: 去目录、去 .exe、转小写 —— 白名单表的键
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

bool looksLikePath(const string &program)
{
    string p = toSlash(program);
    if (p.find('/') != string::npos)
        return true;
    return p.size() > 1 && p[1] == ':';
}
} // namespace

JZShellPolicy::JZShellPolicy()
{
    buildDefaultPrograms();
}

string JZShellPolicy::effectiveBase(const string &base) const
{
    if (!base.empty())
        return base;
    return m_baseCwd.empty() ? m_config.zone.workspace : m_baseCwd;
}

void JZShellPolicy::setConfig(const JZVirtualTerminalConfig &config)
{
    m_config = config;

    // 程序映射(programPaths): ① 白名单里没有的补一条(否则连名字都过不了审)
    //   ② 记下全路径供上层查询。真正的"短名 -> 全路径"解析在 JZShell::resolveProgram()
    //   里做, 命令串不再被改写。
    for (const auto &kv : m_config.programPaths)
    {
        string name = str_toLower(kv.first);
        if (name.empty() || kv.second.empty())
            continue;

        if (!m_programs.count(programKey(name)))
        {
            string base = toSlash(kv.second);
            size_t pos = base.find_last_of('/');
            addProgram(name, { pos == string::npos ? base : base.substr(pos + 1) },
                       CmdCategory::Tool);
        }
        // 映射不等于授权: 只记全路径, 不把它所在目录并进授信目录
        m_trustedPrograms[programKey(name)] = kv.second;
    }
}

void JZShellPolicy::setTrustedPrograms(const std::vector<TrustedProgram> &programs)
{
    m_trustedPrograms.clear();
    for (const TrustedProgram &p : programs)
        m_trustedPrograms[programKey(p.name)] = p.path;
}

void JZShellPolicy::addProgram(const string &name, vector<string> exeNames, CmdCategory category,
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

vector<string> JZShellPolicy::availablePrograms() const
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

JZShellPolicy::Verdict JZShellPolicy::checkReadIn(const JZVirtualTerminalConfig &cfg,
                                                 const string &path)
{
    Verdict v;
    string abs = codeNormalizePath(path);
    string error;
    if (!zoneCheckAccess(cfg, abs, FileAccess::Read, error))
    {
        v.decision = Decision::Deny;
        v.reason = DenyReason::AccessDenied;
        v.detail = error;
        return v;
    }

    v.decision = Decision::Allow;
    v.detail = string("区域: ") + zoneText(zoneOfPath(cfg.zone, abs));
    return v;
}

JZShellPolicy::Verdict JZShellPolicy::checkRead(const string &path) const
{
    return checkReadIn(m_config, path);
}

JZShellPolicy::Verdict JZShellPolicy::checkWriteIn(const JZVirtualTerminalConfig &cfg,
                                                  const string &path)
{
    Verdict v;
    string abs = codeNormalizePath(path);
    string error;
    if (!zoneCheckAccess(cfg, abs, FileAccess::Write, error))
    {
        v.decision = Decision::Deny;
        v.reason = DenyReason::WriteDenied;
        v.detail = error;
        return v;
    }

    v.decision = Decision::Allow;
    v.detail = string("可写, 区域: ") + zoneText(zoneOfPath(cfg.zone, abs));
    return v;
}

JZShellPolicy::Verdict JZShellPolicy::checkWrite(const string &path) const
{
    return checkWriteIn(m_config, path);
}

JZShellPolicy::Verdict JZShellPolicy::checkExec(const string &program) const
{
    return checkExec(program, vector<string>());
}

JZShellPolicy::Verdict JZShellPolicy::checkExec(const string &program,
                                                const vector<string> &args,
                                                const string &base) const
{
    Verdict v;
    string cwd = effectiveBase(base);

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

const char *JZShellPolicy::dangerousArgReason(const string &program,
                                              const vector<string> &args,
                                              const string &base) const
{
    string prog = str_toLower(program);

    // kill: 默认放行, 但只许杀自己启动的进程
    if (prog == "kill" || prog == "killall" || prog == "pkill")
    {
        // 终端不再跟踪进程号(外部命令由 JZShell 自己起/收), 判不出归属 —— 一律不干,
        // 并把正确的做法告诉模型: 停自己起的命令用 terminal_cancel。
        if (prog != "kill")
            return "终端不跟踪进程号, 无法判定归属: 停自己的命令请用 terminal_cancel";

        for (const string &a : args)
        {
            if (a.empty() || a[0] == '-')
                continue;   // -9 / -TERM 等信号 / -l
            return "终端不跟踪进程号, 无法判定这个 pid 是不是自己起的: 停自己的命令请用 terminal_cancel";
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
            string abs = codeNormalizePath(isAbs ? a : (effectiveBase(base) + "/" + a));
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

// ==========================================================================
// 安全底线: 不论哪个权限等级都始终生效
//   1) 特别危险的指令(关机 / 格式化 / 往块设备写 ...)
//   2) 删除工作区外的文件(rm / rmdir 的目标不在工作区树内)
// ⚠ 只覆盖"能静态解析出来的命令"(顶层 / 管道 / 逻辑链, 以及 heredoc 之后同一行里的部分):
//   藏在 $( ) / 反引号 / 进程替换 / 子 shell / heredoc body 里的 rm 抽不出来 ——
//   这是"看不见按可信处理"的**已知盲区**(口径见 JZShellParser.h 顶部注释)。
//   真正的安全边界仍然是沙箱/容器, 这里只是最后一道"明显自杀动作"的闸。
// ==========================================================================

const char *JZShellPolicy::criticalDangerReason(const string &program,
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

bool JZShellPolicy::isDeleteCommand(const string &programLower) const
{
    return programLower == "rm" || programLower == "rmdir" ||
           programLower == "rmtrash" || programLower == "trash" ||
           programLower == "trash-put";
}

bool JZShellPolicy::isWithinWorkspace(const string &absPath) const
{
    string root = codeNormalizePath(m_config.zone.workspace);
    if (root.empty())
        return true;   // 没配工作区: 无法界定"区外", 按信任档放行
    string abs = codeNormalizePath(absPath);
    return abs == root || codeIsSubPath(root, abs);
}

JZShellPolicy::Verdict JZShellPolicy::checkCriticalFloor(const jzsh::AuditCommand &cmd,
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
        string cwd = effectiveBase(base);
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

bool JZShellPolicy::resolvePath(const string &path, bool writeAccess,
                                string &absPath, Verdict &verdict, string &error,
                                const string &base) const
{
    string cwd = effectiveBase(base);

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
// 默认程序白名单
// ==========================================================================

void JZShellPolicy::buildDefaultPrograms()
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

    // 纯语义内建(bash builtin): 不启动任何进程、不走 PATH, 只改当前 shell 的状态。
    // ⚠ 以前漏登记 ⇒ 审核把它们当"外部程序"查白名单, 直接拒:
    //   实测 agent 写 `cd ... && ... ; exit` 被拦成「程序不在白名单里: exit」, 白费一轮。
    //   `set -euo pipefail` / `while read` / `declare -i` 是 agent 的高频写法。
    // ⚠ eval / exec / trap / alias / unalias / getopts / let / times 同样是内建, 但
    //   eval/exec 能拼出审核看不见的命令、trap 会改信号处理 —— 这几条单独议, 不在这里放行。
    static const char *kPureBuiltins[] = {
        "exit", "read", "set", "declare", "local", "return", "shift", "shopt",
    };
    for (const char *cmd : kPureBuiltins)
        addProgram(cmd, { cmd }, CmdCategory::Shell);
}
