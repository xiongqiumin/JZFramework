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
// 上面这套是"默认档(standard)"的取值。另有 strict / loose 两档, 见下面的
// PermissionProfile: 档位只是预设, applyProfile() 展开成上面这些字段。
// 三档共同的**安全底线**(提权 / 改账号 / 改系统设置 / 关机 / 格式化 / 删工作区外)
// 与档位无关, 永远生效。
//
// 命令策略 = **黑名单口径**(2026-11-12 按用户要求从白名单放宽):
//   默认允许(表里没登记的程序也允许, 见 allowUnknownPrograms), 只拦两类:
//     ① 特别危险: 关机 / 格式化 / 提权 / 删工作区外的文件(dd / mkfs / sudo / shutdown ...)
//     ② 改变环境: 安装/升级/卸载依赖(npm install / pip install / apt ...)与改系统设置的
//        命令(reg / netsh / sc ...)
//   "能不能跑"由 shell 的 PATH 决定; 这里只管"该不该跑"。
//
// ⚠ 这不是安全边界(能跑的程序就等于能执行任意代码), 它管的是副作用收敛
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

// ---------------------------------------------------------------------------
// 权限档位(三个预设)
//
// 档位只是**预设值**: applyProfile() 把下面那组字段填一遍, 判定层(checkExec /
// zoneCheckAccess / checkCriticalFloor)永远只看字段, 不看档位 —— 所以加档位/改档位
// 只动 applyProfile 一张表, 不必改判定逻辑。
//
//   严格 strict   : 只跑登记过的命令(白名单口径) + 区外只读 + 禁联网 + 禁装依赖
//   默认 standard : 未登记命令放行(黑名单口径) + 区外只读 + 允许联网 + 禁装依赖
//   宽松 loose    : 默认 + 允许装/卸依赖(授信目录也可写)
//
// 三档共同的"安全底线"(与档位无关, 没有开关): 提权 / 改账号 / 改系统设置 /
// 关机 / 分区格式化 / 往块设备写 / 删除工作区外的文件 —— 见
// JZShellPolicy::checkCriticalFloor。
// ---------------------------------------------------------------------------
enum class PermissionProfile
{
    Strict = 0,
    Standard,
    Loose,
};

// "strict" / "standard" / "loose"(web / 配置 / 日志共用一份文案)
const char *permissionProfileText(PermissionProfile p);
bool permissionProfileFromText(const std::string &text, PermissionProfile &out);

struct JZVirtualTerminalConfig
{
    // ---- 权限档位 ----
    // ⚠ 只用于 applyProfile() 填默认值; 想在某一档上单独放开/收紧某项, 直接改下面
    //   对应的字段即可(applyProfile 之后再赋值), 判定层读的永远是字段。
    PermissionProfile profile = PermissionProfile::Standard;
    void applyProfile(PermissionProfile p);

    // ---- 文件访问: 按区域分档(位组合 FileAccess::*) ----
    // ⚠ 下面这些字段才是判定依据, 默认值 = PermissionProfile::Standard 展开的结果。
    int workspaceAccess = FileAccess::All;                  // 工作区: 读 + 写 + 执行
    int trustedAccess   = FileAccess::Read | FileAccess::Exec;  // 授信目录: 默认只读可执行
    // 区外: 只读(编译要读系统头/库)。
    // 例外: 系统临时目录额外补一个 Write 位(见 zoneIsTempPath) —— 写临时文件不该要特权。
    int outsideAccess   = FileAccess::Read;

    // ---- 命令策略 ----
    // 未登记的程序默认放行(黑名单口径): `ipconfig` / `uptime` / `netstat` / `cmake` 这类
    // 没有登记在默认表里的命令不再被"程序不在白名单里"拦掉。
    // 关掉它 = 回到旧的白名单口径(只有表里登记过的程序能跑)。
    bool allowUnknownPrograms = true;
    bool allowPackageManager = false;   // 默认关: 安装/升级/卸载依赖一律拒绝(程序本身可用)
    bool allowNetwork = true;           // 默认开: 下载/联网本身不改环境, 放行
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

    // ---- 基础环境变量(导出给子进程) ----
    // 与脚本里的 `export FOO=bar` 等效: 之后每条外部命令都能看到。
    // 典型用途: 把"评测输入文件路径"这类上下文交给运行时(`JZ_CASE_INPUT`),
    // 免得把输入写进工程目录(模型会删、会顺着 `..` 翻别人的)。
    std::map<std::string, std::string> env;

    // ---- 目录边界 ----
    ZoneConfig zone;

    // 某个绝对路径适用哪些权限。授信目录按 TrustedDir.writable 单独加写权限。
    int accessOfPath(const std::string &absPath) const;
    int accessOfZone(Zone z, const std::string &absPath) const;
};

const char *zoneText(Zone zone);

// ---------------------------------------------------------------------------
// bash(MSYS / Git-Bash) 风格路径 -> 真实 Windows 路径
//
// ⚠ **一处实现, 两处消费**: ① 权限判定(zoneCheckPath) ② 文件工具(JZCodeTool::fullPath 等)。
//   以前只有 ① 有这张表、② 没有 —— 于是 `/c/tmp/x` 在终端里有效、在 read_file 里被算成
//   `C:\c\tmp\x` 而恒"文件不存在"(实测模型据此认定"构建没产出", 反复重跑烧到轮次上限)。
//
// 表按 Git for Windows **实测**(`cygpath --windows`)写, 不是猜的:
//   /            -> <GitRoot>(如 C:/Program Files/Git)
//   /usr /usr/bin /usr/lib /usr/share  -> <GitRoot>/usr...
//   /bin         -> <GitRoot>/usr/bin(软链到 /usr/bin)
//   /etc /var    -> <GitRoot>/etc | <GitRoot>/var
//   /home[/<u>]  -> <GitRoot>/home...  ⚠ 实测**不是** C:\Users; 只有 `~` 才走 %USERPROFILE%
//   /tmp[/...]   -> 系统临时目录(TEMP/TMP/TMPDIR, 见 zoneTempDirs) = %TEMP%
//   /c[/...]     -> C:/...(裸 `/c` 就是 C:\ 根; **不认** /cygdrive/c ——
//                   Git Bash(MSYS2) 没有这个挂载点, 实测 `cd /cygdrive/c` 报错)
//   /dev/*、/proc/* -> MSYS 虚拟设备/纯内存伪文件系统(硬盘上不存在) ⇒ 不翻, 报错时给提示
// 翻不出来(如找不到 GitRoot)就原样返回 —— 宁可不翻, 也别编一个不存在的路径。
// ---------------------------------------------------------------------------
std::string codeBashPathToWindows(const std::string &input,
                                  const std::vector<std::string> &tempDirs = std::vector<std::string>());

// 看起来像 bash 风格绝对路径吗(`/...` 且非盘符写法)—— 报错时据此给可操作提示。
bool codeLooksLikeBashPath(const std::string &input);

// 系统临时目录(TEMP / TMP / TMPDIR / C:/Windows/Temp, 去重)—— Windows 上就是 bash 的 /tmp
const std::vector<std::string> &zoneTempDirs();

// 判定用的路径规范化: 统一分隔符 + (Windows 上)把 bash 风格路径翻成真实 Windows 路径。
// 凡是"拿路径定区域/定权限"的地方都走这里, 免得各处判断不一致。
std::string zoneCheckPath(const std::string &path);

// 判定一个绝对路径属于哪个区域
Zone zoneOfPath(const ZoneConfig &cfg, const std::string &absPath);

// 解析路径并判定区域: 相对路径基于 cwd。
// ⚠ 区外**不算失败**(区外默认只读) —— 能不能读/写由 zoneCheckAccess() 按意图判,
//   这样 `cat /etc/hosts` 能读、`echo x > /etc/hosts` 被拒, 而不是一律"路径越界"。
// strict=false("全部不检查"那档): 连"工作区未配置"与".. 越界"也不算失败。
bool zoneResolvePath(const ZoneConfig &cfg, const std::string &cwd, const std::string &input,
                     std::string &absPath, Zone &zone, std::string &error, bool strict = true);

// 统一权限判定: need 是 FileAccess 的位组合, 缺哪一位就报错(并返回该区域的权限说明)。
bool zoneCheckAccess(const JZVirtualTerminalConfig &cfg, const std::string &absPath,
                     int need, std::string &error);

// ---------------------------------------------------------------------------
// 命令策略类型(2026-11-09 从 JZCodeTools.h 挪到这里)
//
// 为什么挪: 审核现在归 **JZShell** 管(它自己执行、自己审), 而 JZShell.h 不能反向
// 依赖 JZCodeTools.h(那个头里有 agent 工具宿主, 会成环)。这些是纯数据/纯枚举,
// 放在这份"策略头"里两边都能用。
// 名字保持不变, 现有调用点(JZCodeTools.h / JZVirtualTerminal.h)照样编译。
// ---------------------------------------------------------------------------

// 工具/命令拒绝原因（结构化）: 让上层区分"真的失败了"与"被策略拦了"
enum class DenyReason
{
    None = 0,           // 非拒绝类失败
    PathOutside,        // 路径越界，不在工作区/授信区内
    AccessDenied,       // 区域策略不允许该操作(如区外只读 → 写被拒)
    NotFound,           // 文件/目录/程序不存在
    WriteDenied,        // 不允许写
    CommandDenied,      // 命令被策略禁止
    ProgramDenied,      // 程序不在允许执行的区域
    EnvChange,          // 会修改环境 / 下载安装
    EditFailed          // 编辑类失败(old_str 不匹配、匹配多处等)
};

// 拒绝原因的中文说明（用于日志与提示）
const char *denyReasonText(DenyReason reason);

// 外部程序的分档。定位是"副作用管控", 不是安全边界。
// ⚠ 表只是**黑名单的登记簿**: 没登记的程序按 allowUnknownPrograms 放行。
enum class CmdCategory
{
    Tool = 0,           // 编译/构建/测试：默认允许
    Shell,              // 基础 shell / coreutils：默认允许
    PackageManager,     // pip/npm/apt 等：程序可用，但命中 denyVerbs(安装/卸载)时拒绝
    Network,            // curl/wget 等：默认允许(下载不改环境)
    SystemAdmin,        // reg/netsh/sc/setx 等：永不放行
    Dangerous           // dd/mkfs/sudo/shutdown/chown 等：永不放行
};

struct ProgramSpec
{
    std::string name;
    std::vector<std::string> exeNames;      // 可执行文件全名（同时兼容有无 .exe）
    CmdCategory category = CmdCategory::Tool;
    std::vector<std::string> denyFlags;     // 禁止出现的参数（前缀匹配）
    std::vector<std::string> denyModules;   // 配合 -m：挡掉 python -m pip 这类
    // 只在**子命令命中**时才拒绝(如 npm 的 install/uninstall), 用于"程序本身要能用、
    // 但它的安装/卸载用法必须拦住"这一类: `npm run build` 放行, `npm install` 拒绝。
    // 空 = 整个程序一律拒绝(apt / brew / winget 这种几乎只做安装的)。
    std::vector<std::string> denyVerbs;
};

// 显式指定的可信程序：用全路径直接定位，优先于在授信目录里按名搜索。
struct TrustedProgram
{
    std::string name;   // 程序名（小写，与 ProgramSpec 的 name 对应）
    std::string path;   // 可执行文件全路径
};

// 审核结论。JZVirtualTerminal::Decision/Verdict 现在是它的别名。
enum class PolicyDecision
{
    Allow,
    Deny,
    Ask,           // 自动策略判不了, 需要人拍板(接 MCP input_required)
};

struct PolicyVerdict
{
    PolicyDecision decision = PolicyDecision::Deny;
    DenyReason reason = DenyReason::None;
    std::string detail;                      // 给人 / 给模型看的原因

    bool allowed() const { return decision == PolicyDecision::Allow; }
    bool needAsk() const { return decision == PolicyDecision::Ask; }
};

#endif // !JZ_VIRTUAL_TERMINAL_CONFIG_H_
