#ifndef JZ_VIRTUAL_TERMINAL_CONFIG_H_
#define JZ_VIRTUAL_TERMINAL_CONFIG_H_

#include <map>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// 虚拟终端的配置: 目录边界 + 权限分档 + 命令策略, 一份说完
//
// 以前这三件事是散的: 目录边界在 ZoneConfig, 命令开关在 ProgramPolicy,
// "区外一律拒绝"又硬编码在 checkRead/checkWrite 里, 想改一条策略得翻三个地方。
// 现在合成 JZVirtualTerminalConfig, 而且**默认构造就是推荐策略**:
//
//   工作区        读 + 写 + 执行  —— 写代码、跑编译产物(./app)、跑脚本
//   授信目录      读 + 执行        —— 放编译器/SDK; 哪个目录要写, 把 TrustedDir.writable 打开
//   系统临时目录   读 + 写         —— /tmp 这类地方, 默认就能写(扔临时文件不需要特殊权限)
//   区外其它      只读             —— 编译要读 /usr/include、/usr/lib; 但不许改系统
//   空设备        一律放行         —— /dev/null(还有 /dev/zero、/dev/stdout|stderr|stdin;
//                                   Windows 侧 NUL): 写它不产生文件副作用, `cmd > /dev/null` 该过
//   ⚠ Windows 上模型给的是 Git Bash 风格路径(`/tmp/x`、`/c/...`、`/dev/null`): 判定前先翻成
//     真实 Windows 路径(zoneCheckPath), 否则 `/tmp/x` 会被当成"区域外"拒掉。
//
// 整档切换另有 AccessLevel(见下): 除了上面这套"按区域分档", 还有一档
//   Unrestricted = 全部不检查(什么都放行)。
//
// ⚠ 这不是安全边界(白名单里有 g++ 就等于能执行任意代码), 它管的是副作用收敛
//   与防误操作; 真正的边界只能是沙箱/容器。
// ---------------------------------------------------------------------------

// 区域: 判定一个路径属于哪一类, 从而决定它适用哪一档权限
enum class Zone
{
    Outside = 0,
    Workspace,
    Trusted
};

// 授信目录: 编译器/SDK 之类的安装目录。默认只读, 需要的单独开写
struct TrustedDir
{
    std::string path;
    bool writable = false;
};

// 目录边界
struct ZoneConfig
{
    std::string workspace;                  // 工作区根
    std::vector<TrustedDir> trustedDirs;    // 授信目录
};

// 权限位(可组合: FileAccess::Read | FileAccess::Write)
namespace FileAccess
{
enum Flags
{
    None  = 0,
    Read  = 1,
    Write = 2,
    Exec  = 4,
    All   = Read | Write | Exec,
};
}

// 权限等级(整档切换)
enum class AccessLevel
{
    // 按区域分档(默认, 推荐): 就是上面那张表, 由下面的
    // workspaceAccess / trustedAccess / outsideAccess 决定每档给哪些位。
    Standard = 0,

    // 全部不检查: 目录边界、读 / 写 / 执行、程序白名单、危险命令(dd / sudo / shutdown ...)、
    // 包管理与联网开关 —— 一律放行, 命令原样交给 shell 跑。
    // ⚠ 语义是"我信任这个终端里跑的一切"(本机单人开发、一次性脚本), 它**不是**安全边界:
    //    开着它就别把终端暴露给不受信任的输入(远端 agent_node 尤其)。
    Unrestricted,
};

struct JZVirtualTerminalConfig
{
    // ---- 权限等级 ----
    // Standard     = 按区域分档
    // Unrestricted = 全部不检查(见 AccessLevel 的注释)
    AccessLevel level = AccessLevel::Standard;
    bool unrestricted() const { return level == AccessLevel::Unrestricted; }

    // ---- 文件访问: 按区域分档(位组合 FileAccess::*) ----
    int workspaceAccess = FileAccess::All;                  // 工作区: 读 + 写 + 执行
    int trustedAccess   = FileAccess::Read | FileAccess::Exec;  // 授信目录: 默认只读可执行
    // 区外: 只读(编译要读系统头/库)。
    // 例外: 系统临时目录额外补一个 Write 位(见 zoneIsTempPath) —— 写临时文件不该要特权。
    int outsideAccess   = FileAccess::Read;

    // ---- 命令策略 ----
    bool allowPackageManager = false;   // 默认关: 不要装/升依赖
    bool allowNetwork = false;          // 默认关: 不要下载
    // ⚠ 危险指令(dd / mkfs / sudo / shutdown ...)与改系统设置的命令(SystemAdmin)
    //   一律拒绝, 不提供开关。

    // ---- 程序映射(运行时可配, 动态登记) ----
    //   程序名 -> 可执行文件全路径。例: {"tsc": "D:/app/ts_compiler/tsc/tsc.exe"}
    //   登记一条映射做两件事(见 JZVirtualTerminal::applyProgramPaths):
    //     ① 该名字补进程序白名单(CmdCategory::Tool) —— 默认表里没有的(如 code_runtime)也能过审;
    //     ② 审过之后, 执行时把命令里这个短名原地换成全路径(见 expandProgramPaths),
    //        所以 `tsc xxx` 这种裸命令名能真正找到程序 —— 不改 PATH, 只改命令本身。
    //   典型用途: 本仓库自带的 tsc / code_runtime 这类"随部署走、通常不在 PATH 里"的程序。
    //   ⚠ 只认短名字: 命令里写全路径走的是**路径审核**(区外一律拒), 这条映射管不到 ——
    //     要按全路径跑, 就把那个目录配进 zone.trustedDirs(映射本身不授信任何目录)。
    //   ⚠ 名字用小写, 与命令里写的名字一致; 路径必须是可执行文件本身(不是目录)。
    std::map<std::string, std::string> programPaths;

    // ---- 目录边界 ----
    ZoneConfig zone;

    // 某个绝对路径适用哪些权限。授信目录按 TrustedDir.writable 单独加写权限。
    // ⚠ level == Unrestricted 时一律返回 FileAccess::All(全部不检查)
    int accessOfPath(const std::string &absPath) const;
    int accessOfZone(Zone z, const std::string &absPath) const;
};

const char *zoneText(Zone zone);

// 判定一个绝对路径属于哪个区域
Zone zoneOfPath(const ZoneConfig &cfg, const std::string &absPath);

// 解析路径并判定区域: 相对路径基于 cwd。
// ⚠ 区外**不算失败**(区外默认只读) —— 能不能读/写由 zoneCheckAccess() 按意图判,
//   这样 `cat /etc/hosts` 能读、`echo x > /etc/hosts` 被拒, 而不是一律"路径越界"。
// strict=false("全部不检查"那档): 连"工作区未配置"与".. 越界"也不算失败。
bool zoneResolvePath(const ZoneConfig &cfg, const std::string &cwd, const std::string &input,
                     std::string &absPath, Zone &zone, std::string &error, bool strict = true);

// 统一权限判定: need 是 FileAccess 的位组合, 缺哪一位就报错(并返回该区域的权限说明)。
// ⚠ cfg.level == Unrestricted 时直接放行(全部不检查)
bool zoneCheckAccess(const JZVirtualTerminalConfig &cfg, const std::string &absPath,
                     int need, std::string &error);

#endif // !JZ_VIRTUAL_TERMINAL_CONFIG_H_
