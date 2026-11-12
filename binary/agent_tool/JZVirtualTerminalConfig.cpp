// 见 JZVirtualTerminalConfig.h: 目录边界判定 + 按区域的权限分档
#include "JZVirtualTerminalConfig.h"

#include "JZStd.h"         // str_endsWith / str_chop / codeIsAbsPath
#include "JZString.h"
#include "JZCodeTools.h"   // codeNormalizePath / codeIsSubPath
#include "JZFileInfo.h"    // codeGitRoot(): 判 <root>/usr/bin 是否存在

#include <cctype>          // isalpha / toupper
#include <cstdlib>         // getenv(TEMP / TMP / TMPDIR / ProgramFiles / USERPROFILE)
#include <cstring>         // strcmp

using namespace std;

namespace
{

// 绝对路径判定: "/xxx" 或盘符 "C:/xxx"(与 JZCodeTools.cpp 里那份同逻辑, 那边是 static)
bool zoneIsAbsPath(const string &path)
{
    if (!path.empty() && path[0] == '/')
        return true;
    return path.size() >= 2 && path[1] == ':';
}

// 根目录规范化: 统一分隔符、消掉 "." / ".."、去掉尾部斜杠
// （保留 "C:/" 这样的盘符根，size()>2 的判断就是为此）
string zoneNormalizeRoot(const string &path)
{
    string r = codeNormalizePath(path);
    while (r.size() > 2 && str_endsWith(r, "/"))
        str_chop(r, 1);
    return r;
}

} // namespace

// 系统临时目录。模型往临时目录扔东西是再正常不过的事(解压源码包、mktemp、中间产物),
// 为它去配授信目录纯属折腾 —— 这里直接放行写, 不需要任何特殊权限。
// ⚠ 只开"写": 执行仍按区外对待(区外的程序不许按路径直接跑, 得走白名单)。
const vector<string> &zoneTempDirs()
{
    // 进程内定一次: 临时目录不会中途变
    static const vector<string> dirs = []() -> vector<string>
    {
        vector<string> out;
        // 去重追加(环境变量和硬编码的默认项可能指向同一个目录)
        auto add = [&out](const char *p)
        {
            if (!p || !*p)
                return;
            string t = zoneNormalizeRoot(p);
            if (t.empty())
                return;
            for (const string &x : out)
            {
                if (x == t)
                    return;
            }
            out.push_back(t);
        };

#ifdef WIN32
        add(std::getenv("TEMP"));
        add(std::getenv("TMP"));
        // Git Bash 的 /tmp 取的是 TMPDIR(设了的话), 两个都算
        add(std::getenv("TMPDIR"));
        add("C:/Windows/Temp");
#else
        add(std::getenv("TMPDIR"));
        // macOS 上 TMPDIR 是用户私有的 /var/folders/..., 而大家说的临时目录是 /tmp
        // (它是指向 /private/tmp 的软链) —— 两个都算
        add("/tmp");
#endif
        return out;
    }();
    return dirs;
}

// Git for Windows 的安装根(找不到返回空)。判定用 `<root>/usr/bin` 存在与否 —— 别的都不够硬。
static string codeGitRoot()
{
    static const string root = []() -> string
    {
        auto ok = [](const string &p) { return !p.empty() && JZFileInfo(p + "/usr/bin").isDir(); };
        const char *env = getenv("JZ_GIT_ROOT");
        if (env && *env && ok(env))
            return zoneNormalizeRoot(env);

        const char *pf = getenv("ProgramFiles");
        if (pf && *pf)
        {
            string c = string(pf) + "/Git";
            if (ok(c))
                return zoneNormalizeRoot(c);
        }
        const char *pf86 = getenv("ProgramFiles(x86)");
        if (pf86 && *pf86)
        {
            string c = string(pf86) + "/Git";
            if (ok(c))
                return zoneNormalizeRoot(c);
        }
        for (const char *c : {"C:/Program Files/Git", "C:/Program Files (x86)/Git"})
        {
            if (ok(c))
                return zoneNormalizeRoot(c);
        }
        return string();
    }();
    return root;
}

// 见头文件里的挂载表。tempDirs 给 /tmp 用(空则不翻 /tmp)。
string codeBashPathToWindows(const string &input, const vector<string> &tempDirs)
{
#ifdef WIN32
    const string &p = input;

    // ~[/...] -> %USERPROFILE%(bash 里 HOME=/c/Users/<用户>) —— 先处理它: `~` 不以 '/' 开头
    if (p == "~" || p.compare(0, 2, "~/") == 0)
    {
        const char *home = getenv("USERPROFILE");
        if (home && *home)
            return zoneNormalizeRoot(home) + p.substr(1);
        return input;
    }

    if (p.size() < 2 || p[0] != '/')
        return input;               // 空 / 相对路径 / 已是 C:/... —— 都不动

    // /tmp[/...] -> 系统临时目录(与 bash 里 cygpath -w /tmp 一致)
    if (!tempDirs.empty())
    {
        if (p == "/tmp")
            return tempDirs.front();
        if (p.compare(0, 5, "/tmp/") == 0)
            return tempDirs.front() + p.substr(4);
    }

    // /c[/...] -> C:/...   (Git Bash 的挂载点形式, **只有**这一种)
    // ⚠ 裸盘符(`/c` = 挂载点)必须给 `C:/` 而不是 `C:`: `C:` 在 Windows 里是"该盘的当前
    //   目录", 拿去判目录多半不存在 —— 于是 `cd /c` 会报 No such file or directory
    //   (Git Bash 里 `cd /c; pwd` 给 `/c`)。
    // ⚠ 不认 `/cygdrive/c`: Git Bash(MSYS2) 没有这个挂载点, `cd /cygdrive/c` 在那里就是
    //   报错; 保留它会让两边行为不一致(实测 bash 5.2.37)。
    if (isalpha((unsigned char)p[1]) && (p.size() == 2 || p[2] == '/'))
        return string(1, (char)toupper((unsigned char)p[1])) + ":" +
               (p.size() == 2 ? string("/") : p.substr(2));

    // Git 虚拟根: /usr... /bin /etc /var /home... -> <GitRoot>/...
    const string git = codeGitRoot();
    if (!git.empty())
    {
        static const char *kMounts[] = {"usr", "bin", "etc", "var", "home"};
        for (const char *m : kMounts)
        {
            const string mm = string("/") + m;
            const string real = (strcmp(m, "bin") == 0) ? "/usr/bin" : mm;   // /bin 是 /usr/bin 的软链
            if (p == mm)
                return git + real;
            if (p.compare(0, mm.size() + 1, mm + "/") == 0)
                return git + real + p.substr(mm.size());
        }
    }
#else
    (void)input;
    (void)tempDirs;
#endif
    return input;                   // /dev/*、/proc/* 这类虚拟路径: 不翻
}

bool codeLooksLikeBashPath(const string &input)
{
    return !input.empty() && input[0] == '/' && !(input.size() >= 2 && input[1] == ':');
}

// 判定用的路径规范化: 统一分隔符 + (Windows 上)把 bash 风格路径翻成真实 Windows 路径。
// 凡是"拿路径定区域/定权限"的地方都走这里, 免得各处判断不一致。
// ⚠ 与文件工具**共用同一份** codeBashPathToWindows(见头文件的挂载表): 不翻的话, 模型写
//   `> /tmp/foo`(bash 里完全合法, 提示词也让它用 /tmp)会被当成"区域外"拒掉;
//   而文件工具那边不翻, `/c/tmp/x` 就会被算成 `C:\c\tmp\x` 而恒"文件不存在"。
// ⚠ 这只影响判定与工具, **不改写真正执行的命令**(shell 自己认这些写法)。
string zoneCheckPath(const string &path)
{
    string p = codeNormalizePath(path);
#ifdef WIN32
    p = codeBashPathToWindows(p, zoneTempDirs());
#endif
    return p;
}

namespace
{

bool zoneIsTempPath(const string &absPath)
{
    string p = zoneCheckPath(absPath);
    for (const string &t : zoneTempDirs())
    {
        if (codeIsSubPath(p, t))
            return true;
    }
    return false;
}

// 空设备 / 标准流设备: 写它们不产生任何文件副作用(丢弃、或打到控制台), 所以不属于
// "区域"该管的事, 一律放行。
// 最常见的就是丢输出: `make > /dev/null 2>&1` / `cmd >/dev/null` —— 以前会被
// "当前策略不允许写区域外" 挡下来, 模型只能改成把它写到临时文件里, 反而更脏。
// Windows 侧由 Git Bash 提供 /dev/null; 另外 DOS 设备名 NUL 在 Windows 任何目录下都是
// 空设备(系统不许建同名文件), 所以单独认一下。
bool zoneIsNullDevice(const string &absPath)
{
    string p = zoneCheckPath(absPath);
    while (p.size() > 1 && str_endsWith(p, "/"))
        str_chop(p, 1);

    static const char *devices[] = {
        "/dev/null", "/dev/zero", "/dev/stdout", "/dev/stderr", "/dev/stdin",
    };
    for (const char *d : devices)
    {
        if (p == d)
            return true;
    }

#ifdef WIN32
    // Windows 的 DOS 设备名大小写不敏感, 且 `> NUL` / `> C:/x/NUL` 都指向空设备
    // (系统不许建名为 NUL 的文件, 所以把 basename 是 nul 的一律当设备看待是安全的)
    string lower = str_toLower(p);
    if (!lower.empty() && lower.back() == ':')
        str_chop(lower, 1);
    size_t slash = lower.find_last_of('/');
    string name = (slash == string::npos) ? lower : lower.substr(slash + 1);
    if (name == "nul")
        return true;
#endif
    return false;
}

} // namespace

const char *zoneText(Zone zone)
{
    switch (zone)
    {
    case Zone::Workspace:   return "工作区";
    case Zone::Trusted:     return "授信目录";
    default:                return "区域外";
    }
}

Zone zoneOfPath(const ZoneConfig &cfg, const string &absPath)
{
    string p = zoneCheckPath(absPath);

    string root = zoneNormalizeRoot(cfg.workspace);
    if (!root.empty() && codeIsSubPath(p, root))
        return Zone::Workspace;

    for (const auto &d : cfg.trustedDirs)
    {
        string t = zoneNormalizeRoot(d.path);
        if (!t.empty() && codeIsSubPath(p, t))
            return Zone::Trusted;
    }
    return Zone::Outside;
}

bool zoneResolvePath(const ZoneConfig &cfg, const string &cwd, const string &input,
                     string &absPath, Zone &zone, string &error, bool strict)
{
    string root = zoneNormalizeRoot(cfg.workspace);
    if (strict && root.empty())
    {
        error = "工作区未配置";
        return false;
    }

    bool escaped = false;
    string p = codeNormalizePath(input, &escaped);
    // 越过起始目录的 ".." 直接拒绝，而不是静默夹住
    if (strict && escaped)
    {
        error = "路径越界，不能超出所在分区的根: " + input;
        return false;
    }

    string base = cwd.empty() ? root : cwd;
    string full;
    if (p.empty() || p == ".")
        full = base;
    else if (zoneIsAbsPath(p))
        full = p;
    else
        full = base + "/" + p;

    full = codeNormalizePath(full);

    // ⚠ 落在区外**不算失败**: 区外的默认权限是"只读"(编译要读 /usr/include),
    // 到底能不能读写, 由调用方拿 zone 去 zoneCheckAccess() 按意图判。
    absPath = full;
    zone = zoneOfPath(cfg, full);
    error.clear();
    return true;
}

// 注意参数叫 z 不叫 zone: 后者会把成员 zone(ZoneConfig) 遮蔽掉
int JZVirtualTerminalConfig::accessOfZone(Zone z, const string &absPath) const
{
    if (z == Zone::Workspace)
        return workspaceAccess;

    if (z == Zone::Trusted)
    {
        int access = trustedAccess;
        // 单个授信目录被显式标记为可写时, 只给它加写权限(读/执行仍按分档)
        string p = codeNormalizePath(absPath);
        for (const auto &d : zone.trustedDirs)
        {
            if (!d.writable)
                continue;
            string t = zoneNormalizeRoot(d.path);
            if (!t.empty() && codeIsSubPath(p, t))
            {
                access |= FileAccess::Write;
                break;
            }
        }
        return access;
    }

    // 区外默认只读; 系统临时目录额外给写(见 zoneIsTempPath) —— 写临时文件不需要特殊权限
    int access = outsideAccess;
    if (zoneIsTempPath(absPath))
        access |= FileAccess::Write;

    // 空设备/标准流(/dev/null 等)不产生文件副作用: 读写执行都放行(见 zoneIsNullDevice)
    if (zoneIsNullDevice(absPath))
        return FileAccess::All;

    return access;
}

int JZVirtualTerminalConfig::accessOfPath(const string &absPath) const
{
    return accessOfZone(zoneOfPath(zone, absPath), absPath);
}

// ===========================================================================
// 权限档位: 三张预设表(判定层不读 profile, 只读下面填出来的字段)
// ===========================================================================
const char *permissionProfileText(PermissionProfile p)
{
    switch (p)
    {
    case PermissionProfile::Strict: return "strict";
    case PermissionProfile::Loose:  return "loose";
    case PermissionProfile::Standard: break;
    }
    return "standard";
}

bool permissionProfileFromText(const string &text, PermissionProfile &out)
{
    const string t = str_toLower(text);
    if (t == "strict" || text == "严格")
    {
        out = PermissionProfile::Strict;
        return true;
    }
    if (t == "standard" || t == "default" || text == "默认")
    {
        out = PermissionProfile::Standard;
        return true;
    }
    if (t == "loose" || t == "relaxed" || text == "宽松")
    {
        out = PermissionProfile::Loose;
        return true;
    }
    return false;
}

void JZVirtualTerminalConfig::applyProfile(PermissionProfile p)
{
    profile = p;

    // 区域权限(三档一致): 全盘可读; 工作区读写执行; 授信目录(SDK/工具链)只读可执行
    workspaceAccess = FileAccess::All;
    trustedAccess   = FileAccess::Read | FileAccess::Exec;
    outsideAccess   = FileAccess::Read;

    switch (p)
    {
    case PermissionProfile::Strict:
        // 白名单口径: 只有登记过的程序能跑; 不联网、不装依赖
        allowUnknownPrograms = false;
        allowPackageManager  = false;
        allowNetwork         = false;
        break;

    case PermissionProfile::Loose:
        // 默认口径 + 允许装/卸依赖; 授信目录也可写
        trustedAccess        = FileAccess::All;
        allowUnknownPrograms = true;
        allowPackageManager  = true;
        allowNetwork         = true;
        break;

    case PermissionProfile::Standard:
    default:
        // 黑名单口径: 未登记命令放行; 联网放行; 装/卸依赖仍拒
        allowUnknownPrograms = true;
        allowPackageManager  = false;
        allowNetwork         = true;
        break;
    }
}

bool zoneCheckAccess(const JZVirtualTerminalConfig &cfg, const string &absPath,
                     int need, string &error)
{
    string p = zoneCheckPath(absPath);
    Zone z = zoneOfPath(cfg.zone, p);
    int access = cfg.accessOfZone(z, p);
    string where = string(zoneText(z));

    error.clear();

    if ((need & FileAccess::Read) && !(access & FileAccess::Read))
    {
        error = "当前策略不允许读" + where + ": " + absPath;
        return false;
    }
    if ((need & FileAccess::Write) && !(access & FileAccess::Write))
    {
        // 写是最容易出事的一类, 报错里直接说清楚能往哪写
        error = "当前策略不允许写" + where +
                "(写只允许工作区、系统临时目录, 或被标记为可写的授信目录): " + absPath;
        return false;
    }
    if ((need & FileAccess::Exec) && !(access & FileAccess::Exec))
    {
        error = "当前策略不允许执行" + where + "的程序: " + absPath;
        return false;
    }
    return true;
}
