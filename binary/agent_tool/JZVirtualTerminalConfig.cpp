// 见 JZVirtualTerminalConfig.h: 目录边界判定 + 按区域的权限分档
#include "JZVirtualTerminalConfig.h"

#include "JZStd.h"         // str_endsWith / str_chop / codeIsAbsPath
#include "JZString.h"
#include "JZCodeTools.h"   // codeNormalizePath / codeIsSubPath

#include <cstdlib>         // getenv(TMPDIR / TEMP)

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

bool zoneIsTempPath(const string &absPath)
{
    string p = codeNormalizePath(absPath);
    for (const string &t : zoneTempDirs())
    {
        if (codeIsSubPath(p, t))
            return true;
    }
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
    string p = codeNormalizePath(absPath);

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
                     string &absPath, Zone &zone, string &error)
{
    string root = zoneNormalizeRoot(cfg.workspace);
    if (root.empty())
    {
        error = "工作区未配置";
        return false;
    }

    bool escaped = false;
    string p = codeNormalizePath(input, &escaped);
    // 越过起始目录的 ".." 直接拒绝，而不是静默夹住
    if (escaped)
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
    return access;
}

int JZVirtualTerminalConfig::accessOfPath(const string &absPath) const
{
    return accessOfZone(zoneOfPath(zone, absPath), absPath);
}

bool zoneCheckAccess(const JZVirtualTerminalConfig &cfg, const string &absPath,
                     int need, string &error)
{
    string p = codeNormalizePath(absPath);
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
