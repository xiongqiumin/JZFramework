#include "JZCodeTools.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "JZDir.h"
#include "JZFile.h"
#include "JZFileInfo.h"
#include "JZPlatform.h"
#include "JZRegExp.h"
#include "JZStd.h"
#include "JZString.h"
#include "JZTextStream.h"
#include "JZNetworkAccessManager.h"
#include "JZNetworkRequest.h"
#include "json/json.h"

using namespace std;

// 说明: 本文件是通用工具库, **不允许** include binary/code_agent 下的任何头。
// 需要宿主提供的能力(工作区/读文件/打包)一律走 ICodeToolHost。

// 读文件统一收口: 工具只管拿内容, "怎么读"(LRU 缓存 / 直读 / 远端传输)由宿主决定
static bool readHostFile(ICodeToolHost *host, const string &abs, string &text, string &error)
{
    if (!host)
    {
        error = "工具没有绑定宿主, 无法读文件";
        return false;
    }
    return host->toolReadFile(abs, text, error);
}

// ==================== 基础工具函数 ====================

// 统一分隔符为 '/'，并消除 "." / ".."。
// escaped 不为空时，记录是否有 ".." 试图越过起始目录（调用方据此判越界，而不是静默夹住）
string codeNormalizePath(const string &path, bool *escaped)
{
    if (escaped)
        *escaped = false;

    string p = path;
    for (size_t i = 0; i < p.size(); i++)
    {
        if (p[i] == '\\')
            p[i] = '/';
    }

    string prefix;
    if (p.size() >= 2 && p[1] == ':')
    {
        prefix = p.substr(0, 2);   // Windows 盘符, 如 "C:"
        p = p.substr(2);
    }
    else if (!p.empty() && p[0] == '/')
    {
        prefix = "/";
        p = p.substr(1);
    }

    vector<string> segs = str_split(p, "/", JZ::SkipEmptyParts);
    vector<string> out;
    out.reserve(segs.size());
    for (const auto &s : segs)
    {
        if (s == ".")
            continue;
        if (s == "..")
        {
            if (!out.empty())
                out.pop_back();
            else if (escaped)
                *escaped = true;
            continue;
        }
        out.push_back(s);
    }

    if (prefix.empty())
        return str_join(out, "/");

    // POSIX 根的 "/" 本身就是分隔符, 不能再补一个(否则 "/tmp" -> "//tmp")
    if (prefix == "/")
        return "/" + str_join(out, "/");

    string res = prefix + "/";   // Windows 盘符: "C:" -> "C:/"
    res += str_join(out, "/");
    return res;
}

static bool codeIsAbsPath(const string &path)
{
    if (!path.empty() && path[0] == '/')
        return true;
    return path.size() >= 2 && path[1] == ':';
}

bool codeIsSubPath(const string &path, const string &root)
{
    string a = str_toLower(path);
    string b = str_toLower(root);
    if (a == b)
        return true;
    if (b.empty())
        return true;
    return a.size() > b.size() && a.compare(0, b.size(), b) == 0 && a[b.size()] == '/';
}

bool codeResolvePath(const string &workspace, const string &input, string &absPath, string &error)
{
    string root = codeNormalizePath(workspace);
    while (root.size() > 2 && str_endsWith(root, "/"))
        str_chop(root, 1);
    if (root.empty())
    {
        error = "工作区未配置";
        return false;
    }

    bool escaped = false;
    string p = codeNormalizePath(input, &escaped);
    // 越过工作区根目录的 ".." 直接拒绝，而不是静默夹到根目录
    if (escaped)
    {
        error = "路径越界，不能超出工作区根目录: " + input;
        return false;
    }

    string full;
    if (p.empty() || p == ".")
        full = root;
    else if (codeIsAbsPath(p))
        full = p;
    else
        full = root + "/" + p;

    full = codeNormalizePath(full);
    if (!codeIsSubPath(full, root))
    {
        error = "路径越界，只允许访问工作区内的文件: " + input + " (工作区: " + root + ")";
        return false;
    }

    absPath = full;
    return true;
}

// 简单通配符匹配，只支持 '*' 与 '?'
static bool globMatchOne(const string &name, const string &pattern)
{
    if (pattern.empty() || pattern == "*")
        return true;

    size_t pi = 0, si = 0;
    size_t star = string::npos, match = 0;
    while (si < name.size())
    {
        if (pi < pattern.size() && (pattern[pi] == '?' || pattern[pi] == name[si]))
        {
            pi++;
            si++;
        }
        else if (pi < pattern.size() && pattern[pi] == '*')
        {
            star = pi++;
            match = si;
        }
        else if (star != string::npos)
        {
            pi = star + 1;
            si = ++match;
        }
        else
        {
            return false;
        }
    }
    while (pi < pattern.size() && pattern[pi] == '*')
        pi++;
    return pi == pattern.size();
}

// file_filter 支持逗号分隔的多个模式，如 "*.cpp,*.h"
static bool fileFilterMatch(const string &fileName, const string &filter, bool caseSensitive)
{
    string name = caseSensitive ? fileName : str_toLower(fileName);
    vector<string> patterns = str_split(caseSensitive ? filter : str_toLower(filter), ",", JZ::SkipEmptyParts);
    if (patterns.empty())
        return true;

    for (const auto &p : patterns)
    {
        string pat = str_trimmed(p);
        if (globMatchOne(name, pat))
            return true;
    }
    return false;
}

static vector<string> readLines(const string &text)
{
    vector<string> lines = str_split(text, "\n", JZ::KeepEmptyParts);
    for (auto &l : lines)
    {
        if (!l.empty() && l.back() == '\r')
            l.pop_back();
    }
    return lines;
}

// 判定字节序列是否为合法 UTF-8（用于区分文本/二进制文件）
// allowTruncatedTail: 采样前缀的**末尾**可能把一个多字节字符切开(如中文 3 字节正好跨在 16KB 边界上)——
// 那是"采样"造成的截断, 不是坏编码, 调用方要允许; 否则正常的中文源码会被判成二进制。
static bool isValidUtf8(const char *data, int size, bool allowTruncatedTail = false)
{
    int i = 0;
    while (i < size)
    {
        unsigned char c = (unsigned char)data[i];
        int extra;
        int lo, hi;
        if (c <= 0x7F)                   { i += 1; continue; }
        else if (c >= 0xC2 && c <= 0xDF) { extra = 1; lo = 0x80; hi = 0xBF; }
        else if (c == 0xE0)              { extra = 2; lo = 0xA0; hi = 0xBF; }
        else if (c >= 0xE1 && c <= 0xEC) { extra = 2; lo = 0x80; hi = 0xBF; }
        else if (c == 0xED)              { extra = 2; lo = 0x80; hi = 0x9F; }
        else if (c >= 0xEE && c <= 0xEF) { extra = 2; lo = 0x80; hi = 0xBF; }
        else if (c == 0xF0)              { extra = 3; lo = 0x90; hi = 0xBF; }
        else if (c >= 0xF1 && c <= 0xF3) { extra = 3; lo = 0x80; hi = 0xBF; }
        else if (c == 0xF4)              { extra = 3; lo = 0x80; hi = 0x8F; }
        else return false;               // 非法首字节

        if (i + extra >= size)
        {
            if (!allowTruncatedTail)
                return false;            // 多字节序列被截断
            // 采样区里已经出现的续字节是否合法: 都合法就只能是被采样边界切断, 不是坏编码
            for (int j = i + 1; j < size; j++)
            {
                unsigned char n = (unsigned char)data[j];
                if (n < 0x80 || n > 0xBF)
                    return false;
            }
            return true;
        }
        for (int j = 1; j <= extra; j++)
        {
            unsigned char n = (unsigned char)data[i + j];
            if (n < lo || n > hi)
                return false;
            lo = 0x80; hi = 0xBF;        // 续字节统一范围
        }
        i += extra + 1;
    }
    return true;
}

// 文本文件判断：采样前若干字节，必须是合法 UTF-8（含纯 ASCII），否则视为二进制；
// 合法 UTF-8 不会出现单独 0x00，故含 '\0' 也直接判二进制。
bool isTextFile(const string &path)
{
    JZFile file;
    if (!file.open(path, "rb"))
        return false;

    JZByteArray head = file.read(16 * 1024);   // 采样足够大的前缀做 UTF-8 合法性判定
    file.close();
    if (head.size() == 0)
        return true;                           // 空文件当作文本

    const char *data = head.data();
    for (int i = 0; i < head.size(); i++)
    {
        if (data[i] == '\0')
            return false;
    }
    return isValidUtf8(data, head.size(), true);   // 采样前缀: 末尾允许被切断(见 isValidUtf8)
}

// 遍历时跳过的目录: 隐藏目录与常见的生成物/依赖目录
static bool isSkipDir(const string &name)
{
    if (name.empty() || name[0] == '.')
        return true;
    static const char *skip_list[] = {
        "build", "out", "bin", "obj", "Debug", "Release", "node_modules",
        "cmake-build-debug", "cmake-build-release", "__pycache__"
    };
    for (const auto &s : skip_list)
    {
        if (name == s)
            return true;
    }
    return false;
}

// ==================== 工作区文件浏览（网页端复用） ====================
bool codeListDir(const string &workspace, const string &rel, vector<string> &out, string &error)
{
    string abs;
    if (!codeResolvePath(workspace, rel, abs, error))
        return false;

    JZDir dir(abs);
    if (!dir.exists())
    {
        error = "目录不存在: " + (rel.empty() ? string(".") : rel);
        return false;
    }

    vector<JZFileInfo> list = dir.entryInfoList(JZDir::Files | JZDir::Dirs | JZDir::NoDotAndDotDot, JZDir::SortName);
    out.clear();
    for (const auto &info : list)
    {
        if (info.isDir())
            out.push_back(info.fileName() + "/");
        else
            out.push_back(info.fileName());
    }
    return true;
}

// ==================== TypeScript 运行时说明（写进 system prompt） ====================
string jzTsRuntimeNotes()
{
    string s;
    s += "TypeScript 运行时边界(本工程跑在 JZ 定制运行时上, 不是 Node; 下面几条与 Node 不同):\n";
    s += "  - 只支持 ESM: 没有 require, 也不认 module.exports / exports。\n";
    s += "  - 内置模块直接写裸名(写 'node:http' 也认)。⭐ 一个名字 = 一个包目录, "
         "**默认导入与具名导入都能用**(以前成员只能挂在模块对象上, 写成具名导入会运行期报错):\n";
    s += "      import http from 'http';                  import { createServer } from 'http';\n";
    s += "      import { DatabaseSync } from 'sqlite';    import fs from 'node:fs';\n";
    s += "    内置名: net / dgram / http / mqtt / ws / url / events / fs / path / process / crypto / "
         "buffer / os / serialport / modbus / sqlite / stream / worker_threads / audio"
         "(控件另有 widget; computer use 另有 computeruse —— 这两个只有具名导出, 没有 default)。\n";
    s += "    宿主能力包(看 exe 编没编, 都能 import, 但要先问 available()): cv(OpenCV 图像处理, "
         "OpenCV.js 形状) / onnx(ONNX 推理) / audio(录音/放音/WAV/重采样: audio.record() / play() / "
         "wavRead() / transcode())。\n";
    s += "  - worker_threads(与 Node 同名): `import { Worker, isMainThread, threadId } from 'worker_threads'`; "
         "worker 侧用 `import { parentPort, workerData } from 'worker_threads'` 的 parentPort.on('message') / "
         "parentPort.postMessage; 入口脚本按**运行根**解析(写 './helper/x.js' 或 'helper/x.js' 都行)。\n";
    s += "  - Web 标准全局与 Node 一致, 运行时启动即有(**不用 import**): fetch / Headers / Response / "
         "WebSocket / URL / URLSearchParams / EventEmitter / Buffer / TextEncoder / TextDecoder / "
         "atob / btoa(EventEmitter/Buffer 是 Node 的, 本运行时也挂全局; 编码靠 Uint8Array.toHex/toBase64)。\n";
    s += "  - 想查某个名字有哪些成员: 直接读工程里的 js_wrapper/<名字>/types/index.d.ts"
         "(裸名声明在同目录 ambient.d.ts; 目录名就是 import 的名字, 不确定就 read_file/search_file 找它)。\n";
    s += "  - 自己的模块用相对路径: `import { foo } from './foo'`(扩展名可省, 按运行根目录解析)。\n";
    s += "  - 退出/保活(Node 的 handle ref 语义, 与 Node 一致): 打开着的套接字/服务端(监听中、连接中)、"
         "ref 着的定时器、跑着的 worker、连着的 modbus 连接、**在途的异步请求(fetch / modbus 读写的 promise "
         "还没结算)**都算「有事要等」, 进程不会退出(与真 Node 实测一致: 只发一个 fetch 不 await 也会等到回包); "
         "全部收掉才退。服务端/连接/worker 用完记得 close()/terminate() —— **不收进程就不会退出**; "
         "确实想让某个句柄不吊住进程(例如临时探针、常驻后台服务)就对它调 unref()"
         "(socket/server/WebSocket/worker/modbus/Timeout 都有)。\n";
    return s;
}

string jzTerminalRunNotes()
{
    string s;
    s += "terminal_run 两种模式: 默认**前台**(结果要等命令自己退出才回, 到 timeout_ms 强杀, 中途取不到输出); "
         "background=true 时**后台**(启动成功就返回 run_id, 不等它结束、也不设超时)。\n";
    s += "  - ⭐ 跑一个**会自己退出**的程序(如 `cd build && code_runtime`)只需一次前台 terminal_run: "
         "它的 stdout/stderr **就在这次回包里**。不要重跑同一条命令, 也不要为它开 background。\n";
    s += "  - terminal_query 是给「还要看更完整日志」用的, 不是「再取一次输出」: 同一个 run_id 反复查, "
         "第二次起默认只回**上次查询之后的增量** —— 看到「日志 0 条 / 只返回上次查询之后的增量」就说明你已经拿到过了, "
         "不要为此重跑命令或换 run_id 再查。\n";
    s += "  - 起服务/常驻进程这类**自己不退出**的命令: 用 background=true 起, 拿返回的 run_id, "
         "之后用 terminal_query 反复查状态与日志(可给 tail / grep); 用完了用 terminal_cancel 停掉。\n";
    s += "  - 别再给服务器那条设一个很长的 timeout_ms 前台干等: 它必然以「命令超时, 已被强制终止」收尾, "
         "而且超时那一刻服务就被杀掉了。\n";
    s += "  - 要并发就在**同一条消息里发多个工具调用**; 分成两条消息发的话, 后一条要等前一条返回"
         "(前台那条不结束就不回) —— 那就等于串行了。\n";
    return s;
}

const char *denyReasonText(DenyReason reason)
{
    switch (reason)
    {
    case DenyReason::PathOutside:   return "路径越界";
    case DenyReason::AccessDenied:  return "该区域不允许此操作";
    case DenyReason::NotFound:      return "目标不存在";
    case DenyReason::WriteDenied:   return "只读区域";
    case DenyReason::CommandDenied: return "命令被禁止";
    case DenyReason::ProgramDenied: return "程序不在允许区域";
    case DenyReason::EnvChange:     return "会修改环境或下载安装";
    case DenyReason::EditFailed:    return "编辑失败";
    default:                        return "调用失败";
    }
}

// ==================== JZCodeTool ====================
JZCodeTool::JZCodeTool()
{
}

JZCodeTool::~JZCodeTool()
{
}

void JZCodeTool::setHost(ICodeToolHost *host)
{
    m_host = host;
}

DenyReason JZCodeTool::checkPathPermission(ToolOperator op, std::string rel_path)
{
    if (!m_host)
        return DenyReason::None;   // 无宿主(测试环境)放行

    // 跟终端用同一份策略: 读按 Read 位(区外默认只读, 所以 read_file 能读系统文件),
    // 写按 Write 位(默认只有工作区)。授信目录的 writable 标记在 accessOfZone 里生效。
    const JZVirtualTerminalConfig &cfg = m_host->toolTerminalConfig();
    string path = fullPath(rel_path);
    int need = (op == ToolOperator::Write) ? FileAccess::Write : FileAccess::Read;

    string error;
    if (zoneCheckAccess(cfg, path, need, error))
        return DenyReason::None;

    // 不在工作区也不在任何授信目录(且该区域不允许此操作) —— 是权限问题, 不是"命令被禁止"
    return (op == ToolOperator::Write) ? DenyReason::WriteDenied : DenyReason::AccessDenied;
}

ParamSchema JZCodeTool::makeParam(const string &name, ParamType type, const string &desc)
{
    ParamSchema p;
    p.name = name;
    p.type = type;
    p.description = desc;
    return p;
}

void JZCodeTool::finish(const ToolCall &call, bool ok, const string &text, DenyReason deny)
{
    ToolCallResult result;
    result.id = call.id;
    result.isOk = ok;
    result.result = text;
    sigResult.emit(result);
}

// "文件不存在" 时补一句可操作提示: 翻译表在配置层(与权限判定共用, 见 JZVirtualTerminalConfig.h)。
// 能翻的(如 `/c/...`、`/tmp/...`)把解析结果也报出来 —— 模型据此就能看懂路径语义;
// 翻不了的(`/usr/...`、`/dev/...`)说明该换成什么写法。
static std::string bashPathHint(const std::string &input)
{
    const string win = codeBashPathToWindows(input, zoneTempDirs());
    if (win != input)
        return "（Git Bash 路径, 已按映射解析为: " + win + "）";
    if (codeLooksLikeBashPath(input))
        return "（这是 bash 风格路径（如 `/usr/...`、`/dev/...`）：在 terminal_run 里有效, 文件工具按 "
               "Windows/工作区路径解析 —— 换成 `C:/...` 或工作区相对路径再试）";
    return string();
}

std::string JZCodeTool::fullPath(std::string path)
{
    // bash 风格路径(`~`、`/c/...`、`/tmp/...`、`/usr/...`)先按 Git Bash 的挂载表翻成真实 Windows
    // 路径 —— 必须在"是不是绝对路径"之前翻: `~` 不以 '/' 开头、`/c/...` 会被当成"当前盘上的 \c\..."。
    // 与权限判定(zoneCheckPath)共用同一份实现, 免得"终端里有效、文件工具里恒不存在"。
    path = codeBashPathToWindows(path, zoneTempDirs());

    if (JZDir::isAbsolutePath(path))
        return JZFileInfo::weaklyCanonicalPath(path);

    string root;
    if (m_host)
        root = m_host->toolTerminalConfig().zone.workspace;
    if (root.empty())
        return JZFileInfo::weaklyCanonicalPath(path);   // 无宿主: 按进程当前目录理解
    return JZFileInfo::weaklyCanonicalPath(root + "/" + path);
}


// ==================== JZSearchContextTool ====================
JZSearchContextTool::JZSearchContextTool()
{
    ToolDefine def;
    def.name = "search_context";
    def.description = "在工作区的代码中搜索文本或正则表达式，返回 \"相对工作区路径:行号:内容\" 形式的匹配列表。" \
                      "path 可以是目录(递归搜索)也可以是单个文件；regex=true 时 pattern 按正则解析。" \
                      "默认跳过隐藏目录和 build/out 等生成目录；二进制 / 超过 2MB / 读取失败的文件会跳过并在结果里注明数量，不会静默漏掉。" \
                      "找**文件名**请用 search_file（本工具是找内容）。";
    def.parameters.push_back(makeParam("pattern", ParamType::String, "要搜索的文本或正则表达式"));
    def.parameters.push_back(makeParam("path", ParamType::String, "在哪个目录或文件下搜索，默认 \".\" 表示整个工作区"));
    def.parameters.push_back(makeParam("file_filter", ParamType::String, "文件名过滤，支持 * ? 与逗号分隔多个模式，如 \"*.cpp,*.h\"，默认 \"*\""));
    def.parameters.push_back(makeParam("case_sensitive", ParamType::Boolean, "是否区分大小写，默认 false"));
    def.parameters.push_back(makeParam("regex", ParamType::Boolean, "pattern 是否按正则表达式解析，默认 false（纯文本搜索）"));
    def.parameters.push_back(makeParam("max_results", ParamType::Number, "最多返回多少条匹配，默认 50"));
    def.parameters.push_back(makeParam("max_files", ParamType::Number, "最多扫描多少个文件，默认 1000，避免大仓库一次扫太多"));
    def.required_parameters.push_back("pattern");
    setToolDefine(def);
}

// 正则只编译一次, 目录搜索与单文件搜索共用同一份
static bool compileSearchReg(const string &pattern, bool caseSensitive, JZRegExp &reg)
{
    int flags = JZRegExp::Global | (caseSensitive ? 0 : JZRegExp::IgnoreCase);
    return reg.compile(JZString::fromStdString(pattern), flags);
}

// 结果里的路径统一写成"相对工作区" —— 这样模型能把返回的路径直接喂回 read_file /
// modify_file。不在工作区内(区外只读授权目录)或没有宿主时, 只能退回绝对路径。
static string workspaceRelPath(ICodeToolHost *host, const string &abs)
{
    string root;
    if (host)
        root = host->toolTerminalConfig().zone.workspace;
    root = codeNormalizePath(root);
    while (root.size() > 2 && str_endsWith(root, "/"))
        str_chop(root, 1);
    if (root.empty())
        return abs;

    string norm = codeNormalizePath(abs);
    if (!codeIsSubPath(norm, root))
        return abs;                                  // 工作区外: 相对路径没有意义

    string rel = norm.substr(root.size());
    while (!rel.empty() && rel[0] == '/')
        rel = rel.substr(1);
    return rel.empty() ? string(".") : rel;
}

// 单次搜索的共享统计(递归里累加): 触顶判断、以及"有多少文件被跳过"的说明都靠它
struct SearchStat
{
    int files = 0;         // 真正读过并搜索过的文件数
    long long bytes = 0;   // 累计读入的字节数
    int skipped = 0;       // 因过大 / 二进制 / 读取失败被跳过的文件数
    bool stopped = false;  // 是否因扫描预算(max_files / maxBytes)触顶提前停止
};

static void searchInDir(const string &absDir, const string &relBase, const string &pattern,
                        const string &fileFilter, bool caseSensitive, const JZRegExp *regexp,
                        int maxResults, int maxFiles, long long maxBytes,
                        vector<string> &out, SearchStat &stat, ICodeToolHost *host)
{
    if (stat.stopped || (int)out.size() >= maxResults)
        return;                                      // 结果已够 / 已停止
    if (stat.files >= maxFiles || stat.bytes >= maxBytes)
    {
        stat.stopped = true;                         // 预算用完 ⇒ 结果可能不全, 要告诉模型
        return;
    }

    JZDir dir(absDir);
    vector<JZFileInfo> list = dir.entryInfoList(JZDir::Files | JZDir::Dirs | JZDir::NoDotAndDotDot, JZDir::SortName);
    for (const auto &info : list)
    {
        if ((int)out.size() >= maxResults)
            return;                                  // 结果够了: 不算"预算触顶"
        if (stat.stopped || stat.files >= maxFiles || stat.bytes >= maxBytes)
        {
            stat.stopped = true;
            return;
        }

        string rel = relBase.empty() ? info.fileName() : relBase + "/" + info.fileName();
        if (info.isDir())
        {
            if (!isSkipDir(info.fileName()))
                searchInDir(info.filePath(), rel, pattern, fileFilter, caseSensitive, regexp,
                            maxResults, maxFiles, maxBytes, out, stat, host);
            continue;
        }

        // 顺序讲究: 先过 file_filter, 再判"太大 / 是二进制"。
        // skipped 是给模型的信号("有该搜而没搜成的"), 被模型自己给的过滤条件排除掉的文件不算漏搜 ——
        // 否则回包会谎报(如 file_filter=*.ts 时把几 MB 的 .dat 也算进"跳过 N 个")。
        if (!fileFilterMatch(info.fileName(), fileFilter, false))
            continue;
        if (info.size() > 2 * 1024 * 1024)
        {
            stat.skipped++;                          // 大文件不搜, 但要让模型知道漏了它
            continue;
        }
        if (!isTextFile(info.filePath()))
        {
            stat.skipped++;
            continue;
        }

        stat.files++;
        string content, readError;
        if (!readHostFile(host, info.filePath(), content, readError))
        {
            stat.skipped++;
            continue;
        }

        stat.bytes += (long long)content.size();
        vector<string> lines = readLines(content);
        string needle = caseSensitive ? pattern : str_toLower(pattern);
        for (int i = 0; i < (int)lines.size(); i++)
        {
            bool hit = false;
            if (regexp)
            {
                hit = regexp->test(JZString::fromStdString(lines[i]));
            }
            else
            {
                string target = caseSensitive ? lines[i] : str_toLower(lines[i]);
                hit = str_indexOf(target, needle) != -1;
            }

            if (hit)
            {
                string line = lines[i];
                if ((int)line.size() > 300)
                    line = line.substr(0, 300) + "...";
                out.push_back(rel + ":" + to_string(i + 1) + ": " + line);
                if ((int)out.size() >= maxResults)
                    return;
            }
        }
    }

    // 这个目录正好走完时触顶(正常退出路径, 不是循环里的提前 return)
    if (stat.files >= maxFiles || stat.bytes >= maxBytes)
        stat.stopped = true;
}

void JZSearchContextTool::call(const ToolCall &params)
{
    string pattern = params.params.get("pattern", "").asString();
    if (pattern.empty())
    {
        finish(params, false, "参数 pattern 不能为空");
        return;
    }
    string rel = params.params.get("path", ".").asString();
    string fileFilter = params.params.get("file_filter", "*").asString();
    bool caseSensitive = params.params.get("case_sensitive", false).asBool();
    bool useRegex = params.params.get("regex", false).asBool();
    int maxResults = params.params.get("max_results", 50).asInt();
    if (maxResults < 1)
        maxResults = 50;
    int maxFiles = params.params.get("max_files", 1000).asInt();
    if (maxFiles < 1)
        maxFiles = 1000;
    long long maxBytes = 64LL * 1024 * 1024;   // 单次搜索最多扫描约 64MB 文件内容，避免大仓库卡死

    // 正则只编译一次(顺带校验): 写错的正则要明确报错, 不能静默变成"没有匹配"
    JZRegExp reg;
    const JZRegExp *regexp = nullptr;
    if (useRegex)
    {
        if (!compileSearchReg(pattern, caseSensitive, reg))
        {
            finish(params, false, "正则表达式无效: " + pattern + " (" + reg.errorString() + ")");
            return;
        }
        regexp = &reg;
    }

    string abs = fullPath(rel);
    DenyReason ret = checkPathPermission(ToolOperator::Read, abs);
    if (ret != DenyReason::None)
    {
        finish(params, false, denyReasonText(ret), DenyReason::PathOutside);
        return;
    }

    JZFileInfo info(abs);
    if (!info.exists())
    {
        finish(params, false, "路径不存在: " + rel, DenyReason::NotFound);
        return;
    }

    // 结果里的路径一律相对工作区(能直接再喂给 read_file / modify_file)
    string outBase = workspaceRelPath(m_host, abs);

    vector<string> results;
    SearchStat stat;
    if (info.isDir())
    {
        searchInDir(abs, outBase == "." ? string() : outBase, pattern, fileFilter, caseSensitive, regexp,
                    maxResults, maxFiles, maxBytes, results, stat, m_host);
    }
    else
    {
        // 显式点了文件名时也要过 file_filter, 别悄悄忽略模型给的过滤条件
        if (!fileFilterMatch(info.fileName(), fileFilter, false))
        {
            finish(params, false, "文件名不匹配 file_filter: " + info.fileName() +
                                  " (file_filter = " + fileFilter + ")");
            return;
        }
        if (!isTextFile(abs))
        {
            finish(params, false, "二进制文件无法搜索: " + rel);
            return;
        }
        if (info.size() > maxBytes)
        {
            finish(params, false, "文件过大(" + to_string(info.size() / (1024 * 1024)) +
                                  "MB)，请改用 read_file 分段查看: " + rel);
            return;
        }

        string content, readError;
        if (!readHostFile(m_host, abs, content, readError))
        {
            finish(params, false, readError);
            return;
        }
        stat.files = 1;
        stat.bytes = (long long)content.size();

        string needle = caseSensitive ? pattern : str_toLower(pattern);
        vector<string> lines = readLines(content);
        for (int i = 0; i < (int)lines.size(); i++)
        {
            bool hit = false;
            if (regexp)
                hit = regexp->test(JZString::fromStdString(lines[i]));
            else
            {
                string target = caseSensitive ? lines[i] : str_toLower(lines[i]);
                hit = str_indexOf(target, needle) != -1;
            }
            if (!hit)
                continue;

            string line = lines[i];
            if ((int)line.size() > 300)
                line = line.substr(0, 300) + "...";
            results.push_back(outBase + ":" + to_string(i + 1) + ": " + line);
            if ((int)results.size() >= maxResults)
                break;
        }
    }

    string text;
    if (results.empty())
    {
        text = "没有找到匹配 \"" + pattern + "\" 的内容（已搜索 " + to_string(stat.files) + " 个文件）。";
    }
    else
    {
        text = "匹配 \"" + pattern + "\" 共 " + to_string(results.size()) + " 处" +
               (results.size() == (size_t)maxResults ?
                    "（已达 max_results 上限，可能还有更多，可缩小 path / file_filter）" : "") + ":\n";
        for (const auto &r : results)
            text += r + "\n";
    }
    if (stat.stopped)
        text += "\n（已扫描 " + to_string(stat.files) + " 个文件 / 约 " +
                to_string(stat.bytes / (1024 * 1024)) + "MB，达到单次搜索上限，结果可能不全；"
                "可缩小 path / file_filter 范围后重试）";
    if (stat.skipped > 0)
        text += "\n（另有 " + to_string(stat.skipped) + " 个文件因二进制内容 / 超过 2MB / 读取失败被跳过）";
    finish(params, true, text);
}

// ==================== JZSearchFileTool (按文件名/路径找文件) ====================
JZSearchFileTool::JZSearchFileTool()
{
    ToolDefine def;
    def.name = "search_file";
    def.description = "按**文件名 / 相对路径**在工作区里找文件，返回相对工作区路径列表(目录带尾 \"/\")。" \
                      "pattern 用通配符 * ?，可逗号分隔多个(如 \"*.d.ts,tsconfig.json\")；命中规则 = 文件名或相对工作区路径任一匹配" \
                      "(所以既能写 \"*.d.ts\"，也能写 \"js_wrapper/*/types/*.d.ts\")。" \
                      "本工具只回路径、不回内容: 找内容用 search_context，看内容用 read_file。" \
                      "默认跳过隐藏目录和 build/out/bin 等生成目录，跳过与触顶都会在结果末尾注明。";
    def.parameters.push_back(makeParam("pattern", ParamType::String, "文件名/路径通配符(* ?)，逗号分隔多个，如 \"*.d.ts,tsconfig.json\""));
    def.parameters.push_back(makeParam("path", ParamType::String, "从哪个目录或文件开始找，默认 \".\" 表示整个工作区"));
    def.parameters.push_back(makeParam("case_sensitive", ParamType::Boolean, "是否区分大小写，默认 false"));
    def.parameters.push_back(makeParam("max_results", ParamType::Number, "最多返回多少个文件/目录，默认 100"));
    def.parameters.push_back(makeParam("max_entries", ParamType::Number, "最多扫描多少个目录项，默认 20000，避免大仓库一次扫太多"));
    def.required_parameters.push_back("pattern");
    setToolDefine(def);
}

// 找文件的扫描统计: 触顶/跳过都要说清楚 —— 否则模型会把"没扫到"当成"不存在"
struct FindStat
{
    int entries = 0;       // 扫过的目录项数(文件 + 目录)
    int dirs = 0;          // 扫过的目录数
    int hitFiles = 0;      // 命中文件数
    int hitDirs = 0;       // 命中目录数
    bool stopped = false;  // 是否因扫描预算(max_entries)触顶提前停止
};

// 记下被跳过的目录名(去重, 最多记若干个) —— 用于在回包里告诉模型"什么被排除了"
static void noteSkippedDir(vector<string> &skipped, const string &name)
{
    if (skipped.size() >= 20)
        return;
    for (const auto &s : skipped)
    {
        if (s == name)
            return;
    }
    skipped.push_back(name);
}

static void findInDir(const string &absDir, const string &relBase, const string &pattern,
                      bool caseSensitive, int maxResults, int maxEntries,
                      vector<string> &out, vector<string> &skippedDirs, FindStat &stat)
{
    if (stat.stopped || (int)out.size() >= maxResults)
        return;
    if (stat.entries >= maxEntries)
    {
        stat.stopped = true;
        return;
    }

    JZDir dir(absDir);
    vector<JZFileInfo> list = dir.entryInfoList(JZDir::Files | JZDir::Dirs | JZDir::NoDotAndDotDot, JZDir::SortName);
    for (const auto &info : list)
    {
        if ((int)out.size() >= maxResults)
            return;                                  // 结果够了: 不算"预算触顶"
        if (stat.entries >= maxEntries)
        {
            stat.stopped = true;
            return;
        }
        stat.entries++;

        const string name = info.fileName();
        const string rel = relBase.empty() ? name : relBase + "/" + name;
        const bool hit = fileFilterMatch(name, pattern, caseSensitive) ||
                         fileFilterMatch(rel, pattern, caseSensitive);

        if (info.isDir())
        {
            if (isSkipDir(name))
            {
                noteSkippedDir(skippedDirs, name);
                continue;
            }
            stat.dirs++;
            if (hit)
            {
                out.push_back(rel + "/");
                stat.hitDirs++;
                if ((int)out.size() >= maxResults)
                    return;
            }
            findInDir(info.filePath(), rel, pattern, caseSensitive, maxResults, maxEntries, out, skippedDirs, stat);
            continue;
        }

        if (hit)
        {
            out.push_back(rel);
            stat.hitFiles++;
        }
    }

    if (stat.entries >= maxEntries)
        stat.stopped = true;
}

void JZSearchFileTool::call(const ToolCall &params)
{
    string pattern = params.params.get("pattern", "").asString();
    if (pattern.empty())
    {
        finish(params, false, "参数 pattern 不能为空");
        return;
    }
    string rel = params.params.get("path", ".").asString();
    bool caseSensitive = params.params.get("case_sensitive", false).asBool();
    int maxResults = params.params.get("max_results", 100).asInt();
    if (maxResults < 1)
        maxResults = 100;
    int maxEntries = params.params.get("max_entries", 20000).asInt();
    if (maxEntries < 1)
        maxEntries = 20000;

    string abs = fullPath(rel);
    DenyReason ret = checkPathPermission(ToolOperator::Read, abs);
    if (ret != DenyReason::None)
    {
        finish(params, false, denyReasonText(ret), DenyReason::PathOutside);
        return;
    }

    JZFileInfo info(abs);
    if (!info.exists())
    {
        finish(params, false, "路径不存在: " + rel, DenyReason::NotFound);
        return;
    }

    // 结果里的路径一律相对工作区(能直接再喂给 read_file / search_context)
    string outBase = workspaceRelPath(m_host, abs);

    vector<string> results;
    vector<string> skippedDirs;
    FindStat stat;
    if (info.isDir())
    {
        findInDir(abs, outBase == "." ? string() : outBase, pattern, caseSensitive,
                  maxResults, maxEntries, results, skippedDirs, stat);
    }
    else
    {
        // 显式点了文件: 名字或路径命中就回它, 否则明确说不匹配(别静默返回空)
        const bool hit = fileFilterMatch(info.fileName(), pattern, caseSensitive) ||
                         fileFilterMatch(outBase, pattern, caseSensitive);
        if (!hit)
        {
            finish(params, false, "文件名/路径不匹配 pattern: " + outBase + " (pattern = " + pattern + ")");
            return;
        }
        stat.entries = 1;
        results.push_back(outBase);
        stat.hitFiles = 1;
    }

    string text;
    if (results.empty())
    {
        text = "没有匹配 \"" + pattern + "\" 的文件或目录（已扫描 " + to_string(stat.entries) +
               " 个目录项 / " + to_string(stat.dirs) + " 个目录）。";
    }
    else
    {
        text = "匹配 \"" + pattern + "\" 共 " + to_string(results.size()) + " 项" +
               "（文件 " + to_string(stat.hitFiles) + " / 目录 " + to_string(stat.hitDirs) + "）:\n";
        for (const auto &r : results)
            text += r + "\n";
        if ((int)results.size() >= maxResults)
            text += "（已达 max_results 上限，可能还有更多，可缩小 path / pattern）";
    }
    if (stat.stopped)
        text += "\n（已扫描 " + to_string(stat.entries) + " 个目录项，达到单次搜索上限，结果可能不全；可缩小 path 后重试）";
    if (!skippedDirs.empty())
    {
        string names;
        for (const auto &s : skippedDirs)
            names += (names.empty() ? "" : ", ") + s;
        text += "\n（已跳过这些生成/隐藏目录: " + names +
                "；要找里面的东西请直接把 path 指到该目录）";
    }
    finish(params, true, text);
}

// ==================== JZReadFileTool ====================
JZReadFileTool::JZReadFileTool()
{
    ToolDefine def;
    def.name = "read_file";
    def.description = "读取工作区中某个文本文件的内容，带行号返回。" \
                      "可以用 start_line / line_count 只读取感兴趣的行范围（行号从 1 开始），" \
                      "避免把大文件整篇灌进上下文；单次最多返回 2000 行，超出请分段读取。" \
                      "二进制文件会被拒绝。读文件优先用本工具而不是 `cat` —— 行号 / 分段 / 编码判定 shell 都给不了。";
    def.parameters.push_back(makeParam("path", ParamType::String, "相对工作区或绝对路径"));
    def.parameters.push_back(makeParam("start_line", ParamType::Number, "起始行号（从 1 开始），默认 1"));
    def.parameters.push_back(makeParam("line_count", ParamType::Number, "读取多少行，0 或省略表示读到文件末尾"));
    def.required_parameters.push_back("path");
    setToolDefine(def);
}

void JZReadFileTool::call(const ToolCall &params)
{
    string rel = params.params.get("path", "").asString();
    if (rel.empty())
    {
        finish(params, false, "参数 path 不能为空");
        return;
    }
    int startLine = params.params.get("start_line", 1).asInt();
    int lineCount = params.params.get("line_count", 0).asInt();
    if (startLine < 1)
        startLine = 1;

    string abs = fullPath(rel);
    DenyReason ret = checkPathPermission(ToolOperator::Read, abs);
    if (ret != DenyReason::None)
    {
        finish(params, false, denyReasonText(ret), DenyReason::PathOutside);
        return;
    }

    JZFileInfo info(abs);
    if (info.isDir())
    {
        // ⚠ 别把"这是个目录"说成"文件不存在": 实测模型 read_file 一个**目录**路径
        //   (js_wrapper/bytes/types) 拿到"文件不存在"后就认定"类型声明不存在", 转头用
        //   65 轮 `cat > /tmp/pN.js` + code_runtime 试错(那 65 轮直接烧掉一个用例)。
        //   把真实情况和下一步都写出来, 它就会去 `ls` 那个目录 —— 这才是它想干的事。
        finish(params, false, "这是个**目录**, 不是文件: " + rel +
                                  "   (列内容: terminal_run 里 `ls " + rel + "`, 或 search_file)",
               DenyReason::NotFound);
        return;
    }
    if (!info.exists() || !info.isFile())
    {
        finish(params, false, "文件不存在: " + rel + bashPathHint(rel), DenyReason::NotFound);
        return;
    }
    if (!isTextFile(abs))
    {
        finish(params, false, "不是文本文件（或含二进制内容）: " + rel);
        return;
    }

    string content, readError;
    if (!readHostFile(m_host, abs, content, readError))
    {
        finish(params, false, readError, DenyReason::NotFound);
        return;
    }

    vector<string> lines = readLines(content);
    int begin = startLine - 1;
    if (begin >= (int)lines.size())
    {
        finish(params, false, "start_line 超出文件行数（共 " + to_string(lines.size()) + " 行）");
        return;
    }

    int end = (lineCount > 0) ? min(begin + lineCount, (int)lines.size()) : (int)lines.size();
    const int kMaxLines = 2000;
    bool truncated = false;
    if (end - begin > kMaxLines)
    {
        end = begin + kMaxLines;
        truncated = true;
    }

    string text = "文件 " + rel + "（共 " + to_string(lines.size()) + " 行）:\n";
    for (int i = begin; i < end; i++)
        text += to_string(i + 1) + ": " + lines[i] + "\n";
    if (truncated)
        text += "... 单次最多返回 " + to_string(kMaxLines) + " 行，请用 start_line / line_count 分段读取\n";

    finish(params, true, text);
}

// ==================== JZWriteFileTool ====================
JZWriteFileTool::JZWriteFileTool()
{
    ToolDefine def;
    def.name = "write_file";
    def.description = "创建文件，或整文件覆盖写入（完全重写，局部修改请用 modify_file）。" \
                      "content 为文件的完整内容；已存在文件会被整体替换。父目录不存在会自动创建。";
    def.parameters.push_back(makeParam("path", ParamType::String, "相对工作区或绝对路径"));
    def.parameters.push_back(makeParam("content", ParamType::String, "文件的完整内容（会整体覆盖旧文件）"));
    def.required_parameters.push_back("path");
    def.required_parameters.push_back("content");
    setToolDefine(def);
}

void JZWriteFileTool::call(const ToolCall &params)
{
    string rel = params.params.get("path", "").asString();
    if (rel.empty())
    {
        finish(params, false, "参数 path 不能为空");
        return;
    }
    string content = params.params.get("content", "").asString();

    string abs = fullPath(rel);
    DenyReason ret = checkPathPermission(ToolOperator::Write, abs);
    if (ret != DenyReason::None)
    {
        finish(params, false, denyReasonText(ret), ret);
        return;
    }

    JZFileInfo info(abs);
    if (info.exists() && info.isDir())
    {
        finish(params, false, "目标是目录, 不能写: " + rel, DenyReason::WriteDenied);
        return;
    }

    bool existed = info.exists();

    // 已存在的文件原来是 CRLF 就保持它 —— 否则一次编辑会把整个文件的行尾改掉
    string out = content;
    if (existed && !out.empty())
    {
        string oldText;
        string readError;
        if (readHostFile(m_host, abs, oldText, readError) &&
            oldText.find("\r\n") != string::npos && out.find("\r\n") == string::npos)
        {
            str_replace(out, "\n", "\r\n");
        }
    }

    string dir = JZFileInfo(abs).path();
    if (!dir.empty())
    {
        JZDir parent;
        if (!parent.exists(dir) && !parent.mkpath(dir))
        {
            finish(params, false, "创建父目录失败: " + dir, DenyReason::WriteDenied);
            return;
        }
    }

    JZFile file;
    if (!file.open(abs, "wb"))
    {
        finish(params, false, "打开文件失败(权限?): " + rel, DenyReason::WriteDenied);
        return;
    }
    int written = file.write(str_toUtf8(out));
    file.close();
    if (written != (int)out.size())
    {
        finish(params, false, "写入长度不符: " + rel, DenyReason::EditFailed);
        return;
    }

    vector<string> lines = readLines(out);
    string msg = (existed ? "已完整覆写文件 " : "已新建文件 ") + rel +
                 "，共 " + to_string(lines.size()) + " 行。";
    finish(params, true, msg);
}

// ==================== JZModifyFileTool ====================
JZModifyFileTool::JZModifyFileTool()
{
    ToolDefine def;
    def.name = "modify_file";
    def.description =
        "对已有文件做局部片段精确替换（不动其它内容）。\n"
        "old_str 必须非空，且**默认要求唯一匹配**文件里的原文并替换为 new_str"
        "(匹配到多处会报错, 这时请补上上下文使它唯一, 或者设 replace_all=true)。\n"
        "整文件写入 / 新建文件请用 write_file；本工具只做片段级修改。\n"
        "改代码务必先看清原文: 空格、换行、缩进必须完全一致。";
    def.parameters.push_back(makeParam("path", ParamType::String, "文件路径(相对工作区或绝对路径)"));
    def.parameters.push_back(makeParam("old_str", ParamType::String,
                                       "要被替换掉的原文(必须非空; 整文件写入请改用 write_file)"));
    def.parameters.push_back(makeParam("new_str", ParamType::String, "替换后的新内容"));
    def.parameters.push_back(makeParam("replace_all", ParamType::Boolean,
                                       "old_str 匹配多处时是否全部替换(默认 false = 报错)"));
    def.required_parameters.push_back("path");
    def.required_parameters.push_back("old_str");
    def.required_parameters.push_back("new_str");
    setToolDefine(def);
}

// 取父目录("a/b/c.txt" -> "a/b"; 没有父目录返回空)
static string jzDirPart(const string &path)
{
    size_t pos = path.find_last_of("/\\");
    if (pos == string::npos)
        return string();
    if (pos == 0)
        return path.substr(0, 1);
    return path.substr(0, pos);
}

// 把命中位置前后几行带行号贴出来 —— 成功后让模型确认改对了地方, 失败时帮它定位
static string makeContextSnippet(const vector<string> &lines, int lineIndex, int radius)
{
    int begin = max(0, lineIndex - radius);
    int end = min((int)lines.size(), lineIndex + radius + 1);
    string text;
    for (int i = begin; i < end; i++)
        text += to_string(i + 1) + ": " + lines[i] + "\n";
    return text;
}

// 字符偏移 -> 行号(0 基)。多行 old_str 也能定位到起始行。
static int lineOfOffset(const string &text, size_t offset)
{
    int line = 0;
    for (size_t i = 0; i < offset && i < text.size(); ++i)
    {
        if (text[i] == '\n')
            line++;
    }
    return line;
}

void JZModifyFileTool::call(const ToolCall &params)
{
    string rel = params.params.get("path", "").asString();
    if (rel.empty())
    {
        finish(params, false, "参数 path 不能为空");
        return;
    }

    string oldStr = params.params.get("old_str", "").asString();
    string newStr = params.params.get("new_str", "").asString();
    bool replaceAll = params.params.get("replace_all", false).asBool();

    string abs = fullPath(rel);

    DenyReason ret = checkPathPermission(ToolOperator::Write, abs);
    if (ret != DenyReason::None)
    {
        finish(params, false, denyReasonText(ret), ret);
        return;
    }

    JZFileInfo info(abs);
    if (info.exists() && info.isDir())
    {
        finish(params, false, "目标是目录, 不能写: " + rel, DenyReason::WriteDenied);
        return;
    }

    // ---- old_str 必须非空: 整文件写入交给 write_file ----
    if (oldStr.empty())
    {
        finish(params, false, "old_str 不能为空：整文件写入 / 新建文件请用 write_file，"
                               "本工具只做片段级替换。", DenyReason::EditFailed);
        return;
    }

    // ---- 精确替换 ----
    if (info.isDir())
    {
        finish(params, false, "这是个**目录**, 不是文件: " + rel +
                                  "   (列内容: terminal_run 里 `ls " + rel + "`)",
               DenyReason::NotFound);
        return;
    }
    if (!info.exists())
    {
        finish(params, false, "文件不存在: " + rel + "  (新建文件请用 write_file)" + bashPathHint(rel),
               DenyReason::NotFound);
        return;
    }
    if (!isTextFile(abs))
    {
        finish(params, false, "不能修改二进制文件: " + rel);
        return;
    }

    string oldContent;
    string readError;
    if (!readHostFile(m_host, abs, oldContent, readError))
    {
        finish(params, false, readError, DenyReason::NotFound);
        return;
    }

    vector<size_t> hits;
    size_t pos = 0;
    while ((pos = oldContent.find(oldStr, pos)) != string::npos)
    {
        hits.push_back(pos);
        pos += oldStr.size();
    }

    if (hits.empty())
    {
        finish(params, false,
               "在 " + rel + " 中找不到 old_str, 请先看清原文(空格/换行/缩进必须完全一致)。",
               DenyReason::EditFailed);
        return;
    }
    if (hits.size() > 1 && !replaceAll)
    {
        // 把前几处命中贴出来 —— 模型看完就知道该补什么上下文
        vector<string> lines = readLines(oldContent);
        string text = "old_str 在 " + rel + " 中匹配了 " + to_string(hits.size()) +
                      " 处, 请补上上下文使它唯一, 或者设 replace_all=true。命中位置:\n";
        int shown = 0;
        for (size_t i = 0; i < hits.size() && shown < 3; ++i, ++shown)
            text += "-- 第 " + to_string(lineOfOffset(oldContent, hits[i]) + 1) + " 行附近:\n" +
                    makeContextSnippet(lines, lineOfOffset(oldContent, hits[i]), 2);
        finish(params, false, text, DenyReason::EditFailed);
        return;
    }

    string newContent = oldContent;
    int replaced = 0;
    if (replaceAll)
    {
        pos = 0;
        while ((pos = newContent.find(oldStr, pos)) != string::npos)
        {
            newContent.replace(pos, oldStr.size(), newStr);
            pos += newStr.size();
            replaced++;
        }
    }
    else
    {
        pos = newContent.find(oldStr);
        newContent.replace(pos, oldStr.size(), newStr);
        replaced = 1;
    }

    // 保留原文件换行风格
    if (!newContent.empty() && oldContent.find("\r\n") != string::npos &&
        newContent.find("\r\n") == string::npos)
    {
        str_replace(newContent, "\n", "\r\n");
    }

    JZFile file;
    if (!file.open(abs, "wb"))
    {
        finish(params, false, "打开文件失败(权限?): " + rel, DenyReason::WriteDenied);
        return;
    }
    int written = file.write(str_toUtf8(newContent));
    file.close();
    if (written != (int)newContent.size())
    {
        finish(params, false, "写入长度不符: " + rel, DenyReason::EditFailed);
        return;
    }

    // 成功也回一段上下文: 让模型自己确认改到了预期的地方
    string text = "已修改 " + rel + ", 替换 " + to_string(replaced) + " 处。改动位置上下文:\n";
    vector<string> lines = readLines(newContent);
    int hitLine = lineOfOffset(newContent, newContent.find(newStr));
    if (hitLine >= 0)
        text += makeContextSnippet(lines, hitLine, 4);

    finish(params, true, text);
}

// ==================== JZGetDateTimeTool ====================
JZGetDateTimeTool::JZGetDateTimeTool()
{
    ToolDefine def;
    def.name = "get_datetime";
    def.description = "获取当前的日期和时间。模型在需要“今天 / 现在 / 星期几”这类信息时，"
                      "应优先调用本工具而不是依赖自身训练数据中的时间。"
                      "默认返回结构化 JSON（含 date/time/weekday/timestamp/datetime 字段）；"
                      "也可传 format 用 strftime 风格字符串（如 \"%Y-%m-%d %H:%M:%S\"）自定义输出。"
                      "utc=true 时返回 UTC 时间而非本地时间。";
    def.parameters.push_back(makeParam("format", ParamType::String,
        "自定义输出格式（strftime 风格，如 \"%Y-%m-%d %H:%M:%S\"）。"
        "留空则返回包含 date/time/weekday/timestamp/datetime 的结构化 JSON。"));
    def.parameters.push_back(makeParam("utc", ParamType::Boolean,
        "是否返回 UTC 时间而非本地时间，默认 false（本地时间）。"));
    setToolDefine(def);
}

void JZGetDateTimeTool::call(const ToolCall &params)
{
    bool utc = params.params.get("utc", false).asBool();
    string fmt = str_trimmed(params.params.get("format", "").asString());

    time_t t = time(nullptr);
    struct tm tm_buf;
    if (utc)
    {
#ifdef _WIN32
        gmtime_s(&tm_buf, &t);
#else
        gmtime_r(&t, &tm_buf);
#endif
    }
    else
    {
#ifdef _WIN32
        localtime_s(&tm_buf, &t);
#else
        localtime_r(&t, &tm_buf);
#endif
    }

    // 自定义格式：直接返回 strftime 结果（单行）
    if (!fmt.empty())
    {
        char buf[256] = {0};
        strftime(buf, sizeof(buf), fmt.c_str(), &tm_buf);
        finish(params, true, string(buf));
        return;
    }

    // 默认：结构化 JSON
    char dateBuf[32] = {0}, timeBuf[32] = {0}, isoBuf[64] = {0};
    strftime(dateBuf, sizeof(dateBuf), "%Y-%m-%d", &tm_buf);
    strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", &tm_buf);
    strftime(isoBuf, sizeof(isoBuf), "%Y-%m-%dT%H:%M:%S", &tm_buf);
    // 星期：本地化中文（tm_wday 0=周日）
    static const char *kWeekday[] = {"星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"};
    int wd = tm_buf.tm_wday % 7;

    Json::Value root;
    root["date"] = dateBuf;
    root["time"] = timeBuf;
    root["weekday"] = kWeekday[wd];
    root["timestamp"] = (Json::Int64)t;
    root["datetime"] = isoBuf;
    Json::FastWriter writer;
    finish(params, true, writer.write(root));
}

// ==================== JZMemoryTool ====================
JZMemoryTool::JZMemoryTool()
{
    ToolDefine def;
    def.name = "memory";
    def.description =
        "跨任务的长期记忆(一个纯文本文件)。agent 每次开工时这份记忆会被自动带进上下文, "
        "所以这里只记\"下次还用得上\"的结论, 不记流水账。\n"
        "action=read    : 看当前记忆的内容(超上限时只返回最近的部分)。\n"
        "action=append  : 追加一条(自动带时间戳)。拿到值得跨任务复用的结论就用它 —— "
        "环境事实(构建/运行命令、程序路径、依赖)、踩过的坑、接口/目录约定、最终结论。\n"
        "action=replace : 整文件重写。只在需要精简或纠正过时结论时用, 重写前先 read, 别丢内容。";
    def.parameters.push_back(makeParam("action", ParamType::String, "read / append / replace"));
    def.parameters.push_back(makeParam("text", ParamType::String,
        "append / replace 要写入的内容(append 一条一句或一小段; replace 是整份新内容)"));
    def.required_parameters.push_back("action");
    setToolDefine(def);
}

void JZMemoryTool::setup(const string &file, int maxBytes)
{
    m_file = file;
    m_maxBytes = maxBytes > 0 ? maxBytes : 8192;
}

// 记忆条目时间戳(本地时间)
static string memoryStamp()
{
    time_t t = time(nullptr);
    struct tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    char buf[64] = {0};
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm_buf);
    return string(buf);
}

bool JZMemoryTool::readRaw(string &text) const
{
    text.clear();
    if (m_file.empty())
        return false;
    JZFileInfo info(m_file);
    if (!info.exists() || !info.isFile())
        return false;

    JZFile file;
    if (!file.open(m_file, "rb"))
        return false;
    JZByteArray data = file.readAll();
    file.close();

    text.assign(data.data(), data.size());
    // 去掉 BOM: 记忆文件是给人看也给人编的, 编辑器存出来的 BOM 别渗进模型上下文
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF &&
        (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
        text = text.substr(3);
    return true;
}

bool JZMemoryTool::writeAll(const string &text, string &error) const
{
    string dir = JZFileInfo(m_file).path();
    if (!dir.empty())
    {
        JZDir parent;
        if (!parent.exists(dir) && !parent.mkpath(dir))
        {
            error = "创建记忆文件所在目录失败: " + dir;
            return false;
        }
    }

    JZFile file;
    if (!file.open(m_file, "wb"))
    {
        error = "打开记忆文件失败(权限?): " + m_file;
        return false;
    }
    int written = file.write(str_toUtf8(text));
    file.close();
    if (written != (int)text.size())
    {
        error = "写入记忆文件长度不符: " + m_file;
        return false;
    }
    return true;
}

string JZMemoryTool::readMemory(int *totalBytes, bool *truncated) const
{
    if (totalBytes)
        *totalBytes = 0;
    if (truncated)
        *truncated = false;

    string text;
    if (!readRaw(text))
        return string();

    while (!text.empty() && (text[text.size() - 1] == '\n' || text[text.size() - 1] == '\r'))
        text.erase(text.size() - 1);

    if (totalBytes)
        *totalBytes = (int)text.size();
    if ((int)text.size() <= m_maxBytes)
        return text;

    // 超上限: 只给最近的(append 语义下新的在后面), 旧的还在文件里, 让它自己去看/去精简
    if (truncated)
        *truncated = true;
    string tail = text.substr(text.size() - m_maxBytes);
    size_t nl = tail.find('\n');
    if (nl != string::npos)
        tail = tail.substr(nl + 1);
    return "(更早的记忆已省略: 共 " + to_string(text.size()) + " 字节, 上限 " + to_string(m_maxBytes) +
           " 字节。旧内容还在 " + m_file + " 里, 可以用 read_file 分段看; 如果这条旧记忆不再有用, "
           "用 memory replace 把它精简掉)\n" + tail;
}

void JZMemoryTool::call(const ToolCall &params)
{
    if (m_file.empty())
    {
        finish(params, false, "记忆功能没有配置存储文件, 不可用");
        return;
    }

    string action = str_toLower(str_trimmed(params.params.get("action", "").asString()));
    string text = params.params.get("text", "").asString();
    if (action.empty())
        action = "read";       // 缺省只读: 写动作必须显式说, 免得误写

    if (action == "read")
    {
        int total = 0;
        bool cut = false;
        string mem = readMemory(&total, &cut);
        if (str_trimmed(mem).empty())
        {
            finish(params, true, "记忆还是空的(" + m_file + ")。"
                                 "后面得到值得跨任务复用的结论时, 用 memory append 写进来。");
            return;
        }
        string head = "记忆文件: " + m_file + "  共 " + to_string(total) + " 字节" +
                      (cut ? " (本次只返回最近的部分)" : "") + "\n";
        finish(params, true, head + mem);
        return;
    }

    if (action == "append" || action == "replace")
    {
        if (str_trimmed(text).empty())
        {
            finish(params, false, "text 不能为空");
            return;
        }

        string body;
        if (action == "append")
        {
            // 记忆每次开工都要注入上下文: 无限长 = 每次任务都白烧一遍 token, 还会把真正
            // 关键的结论挤到省略区外面。所以超过硬上限就拒绝追加, 逼模型先精简 ——
            // 这一次"什么才算关键"的取舍, 比继续往上堆有价值。
            string raw;
            readRaw(raw);
            long long hard = (long long)m_maxBytes * 4;
            if ((long long)raw.size() > hard)
            {
                finish(params, false, "记忆文件已 " + to_string(raw.size()) + " 字节, 超过上限 " +
                                          to_string(hard) + " 字节。请先 read, 再用 memory replace "
                                          "精简成条目式结论(合并重复的、删掉过时的), 然后继续。");
                return;
            }

            while (!raw.empty() && (raw[raw.size() - 1] == '\n' || raw[raw.size() - 1] == '\r'))
                raw.erase(raw.size() - 1);

            string one = text;
            str_replace(one, "\r\n", "\n");
            str_replace(one, "\n", "\n  ");     // 多行缩进两格: 一条一块, 注入时好读
            string entry = "- [" + memoryStamp() + "] " + one;
            body = raw.empty() ? entry : (raw + "\n" + entry);
            body += "\n";
        }
        else
        {
            body = text;
            if (!body.empty() && body[body.size() - 1] != '\n')
                body += "\n";
        }

        string error;
        if (!writeAll(body, error))
        {
            finish(params, false, error, DenyReason::WriteDenied);
            return;
        }

        string msg = (action == "append" ? "已追加到记忆" : "已重写记忆");
        msg += "(" + m_file + ", 现在 " + to_string(body.size()) + " 字节)";
        if ((int)body.size() > m_maxBytes)
            msg += "。⚠ 已超过注入上限 " + to_string(m_maxBytes) + " 字节, 超出的旧内容不会被自动带进上下文, "
                   "建议用 memory replace 精简";
        finish(params, true, msg);
        return;
    }

    finish(params, false, "action 必须是 read / append / replace");
}

// ==================== JZBuildProjectTool ====================
JZBuildProjectTool::JZBuildProjectTool()
{
    ToolDefine def;
    def.name = "build";
    def.description = "编译当前工作区的 TS 工程, 等于在工作区执行 tsc";
    setToolDefine(def);
}

void JZBuildProjectTool::call(const ToolCall &params)
{
    if (!m_host)
    {
        finish(params, false, "工具没有绑定宿主，无法打包");
        return;
    }

    // 打包是宿主可选能力：不提供结束信号的宿主(例如远端 agent_node)直接拒绝
    JZSignal<bool, const std::string &> *buildSignal = m_host->toolBuildFinished();
    if (!buildSignal)
    {
        finish(params, false, "当前宿主不支持打包/发布");
        return;
    }

    if (m_host->toolIsBuildRunning())
    {
        finish(params, false, "已有任务进行中，请等待其完成后再试");
        return;
    }

    if (!m_connected)
    {
        m_connected = true;
        connect(buildSignal, this,
                [this](bool ok, const std::string &error)
                {
                    onBuildDone(ok, error);
                });
    }

    string error;
    if (!m_host->toolBuildProject(error))
    {
        finish(params, false, error);
        return;
    }
    m_pending = params;   // 编译结束（sigBuildFinished）后回填结果
}

void JZBuildProjectTool::cancel(const std::string &id)
{
    if (id == m_pending.id)
        m_pending = ToolCall();   // 丢弃待回填结果；编译进程由 agent 的 abortBuild 负责
}

void JZBuildProjectTool::onBuildDone(bool ok, const std::string &error)
{
    if (m_pending.id.empty())
        return;

    ToolCall call = m_pending;
    m_pending = ToolCall();
    if (ok)
        finish(call, true, "编译通过，运行: 在产物目录跑 `code_runtime`（缺省入口 main.js）。");
    else
        finish(call, false, error);
}


bool splitCommandStrict(const string &cmd, vector<string> &argv, string &error)
{
    argv.clear();
    string cur;
    bool inQuote = false;
    char quoteChar = 0;

    for (size_t i = 0; i < cmd.size(); i++)
    {
        char c = cmd[i];
        if (inQuote)
        {
            // 引号内不做任何解释，整段作为参数的一部分
            if (c == quoteChar)
                inQuote = false;
            else
                cur.push_back(c);
        }
        else
        {
            if (c == '"' || c == '\'')
            {
                inQuote = true;
                quoteChar = c;
            }
            else if (c == ' ' || c == '\t')
            {
                if (!cur.empty())
                {
                    argv.push_back(cur);
                    cur.clear();
                }
            }
            else if (c == '&' || c == '|' || c == '<' || c == '>' || c == '^')
            {
                error = "命令中不允许出现 '" + string(1, c) +
                        "'（虚拟终端不支持管道、重定向与命令串联，请勿换用其它写法重试）";
                return false;
            }
            else if (c == '\r' || c == '\n')
            {
                error = "命令不支持换行";
                return false;
            }
            else
            {
                cur.push_back(c);
            }
        }
    }

    if (inQuote)
    {
        error = "引号未闭合: " + cmd;
        return false;
    }
    if (!cur.empty())
        argv.push_back(cur);
    return true;
}

// ==================== JZWebSearchTool ====================
// 查询词 URL 编码（application/x-www-form-urlencoded 风格）
static string urlEncodeQuery(const string &s)
{
    static const char *hex = "0123456789ABCDEF";
    string out;
    for (unsigned char c : s)
    {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~')
            out.push_back((char)c);
        else if (c == ' ')
            out.push_back('+');
        else
        {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

// 在 JSON 对象里按多个候选 key 取第一个非空字符串
static string searchField(const Json::Value &obj, const std::vector<string> &keys)
{
    for (const auto &k : keys)
    {
        if (obj.isMember(k) && obj[k].isString())
        {
            string v = obj[k].asString();
            if (!v.empty())
                return v;
        }
    }
    return string();
}

// 默认搜索源：必应网页搜索的 RSS 输出（常规网页结果、无需 API Key、中英文通用）。
// 默认仅此一个内置源；如需多源自动回退，在 call() 里追加更多候选 URL 即可。
static const char *kBingRssSearchUrl = "https://www.bing.com/search?q={q}&format=rss&count=15";
// 接近浏览器的 UA，提高无 key 网页搜索接口（必应 RSS）的放行率
static const char *kSearchUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/126.0 Safari/537.36";

// 全量替换子串（from 为空时原样返回）
static void replaceAllStr(string &s, const string &from, const string &to)
{
    if (from.empty())
        return;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != string::npos)
    {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

// HTML 实体反转义（注意 &amp; 最后替换，避免二次转义）
static string htmlUnescapeStr(const string &s)
{
    string out = s;
    replaceAllStr(out, "&lt;", "<");
    replaceAllStr(out, "&gt;", ">");
    replaceAllStr(out, "&quot;", "\"");
    replaceAllStr(out, "&apos;", "'");
    replaceAllStr(out, "&#39;", "'");
    replaceAllStr(out, "&#x27;", "'");
    replaceAllStr(out, "&nbsp;", " ");
    replaceAllStr(out, "&amp;", "&");
    return out;
}

// 去掉 HTML 标签并按空白折叠（description 片段用）
static string stripHtmlTags(const string &s)
{
    string out;
    out.reserve(s.size());
    bool inTag = false;
    for (unsigned char c : s)
    {
        if (c == '<')
            inTag = true;
        else if (c == '>')
            inTag = false;
        else if (!inTag)
            out.push_back((char)c);
    }
    string folded;
    folded.reserve(out.size());
    bool prevSpace = false;
    for (unsigned char c : out)
    {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
        {
            if (!prevSpace)
                folded.push_back(' ');
            prevSpace = true;
        }
        else
        {
            folded.push_back((char)c);
            prevSpace = false;
        }
    }
    return str_trimmed(folded);
}

// 取 <tag>..</tag> 的内容（无属性元素；支持 <![CDATA[ .. ]]>）
static string rssTagContent(const string &xml, const string &tag)
{
    string open = "<" + tag + ">";
    string close = "</" + tag + ">";
    size_t b = xml.find(open);
    if (b == string::npos)
        return string();
    b += open.size();
    size_t e = xml.find(close, b);
    if (e == string::npos)
        return string();
    string v = xml.substr(b, e - b);
    const string CD = "<![CDATA[";
    if (v.find(CD) == 0 && v.size() >= CD.size() + 3 && v.rfind("]]>") == v.size() - 3)
        v = v.substr(CD.size(), v.size() - CD.size() - 3);
    return v;
}

// 是否 RSS/Atom 类 XML（含 <rss> 或 <feed>）
static bool looksRssFeed(const string &body)
{
    return body.find("<rss") != string::npos || body.find("<feed") != string::npos;
}

// 从 RSS/Atom body 里收集 <item>/<entry> 结果（与 JSON 源共用输出格式）
static void collectRssResults(const string &body, vector<string> &out)
{
    size_t pos = 0;
    while (pos < body.size())
    {
        size_t itemBeg = body.find("<item", pos);
        if (itemBeg == string::npos)
            break;
        size_t itemEnd = body.find("</item>", itemBeg);
        if (itemEnd == string::npos)
            break;
        string block = body.substr(itemBeg, itemEnd - itemBeg);
        pos = itemEnd + 7;

        string title = htmlUnescapeStr(rssTagContent(block, "title"));
        string link  = htmlUnescapeStr(rssTagContent(block, "link"));
        string desc  = stripHtmlTags(htmlUnescapeStr(rssTagContent(block, "description")));
        if (title.empty() && link.empty() && desc.empty())
            continue;

        string line;
        if (!title.empty())
            line += title + "\n";
        if (!link.empty())
            line += "  " + link + "\n";
        if (!desc.empty())
            line += "  " + desc + "\n";
        if (!line.empty())
            out.push_back(line);
    }
}

// 把收集到的条目拼成最终返回文本（0 条时给出友好提示）
static string formatSearchResults(const vector<string> &results, int num)
{
    if (results.empty())
        return "未从搜索服务获取到结果（可能无匹配，或服务返回了空列表）。";

    int n = min(num, (int)results.size());
    string text = "网络搜索结果（共 " + to_string(results.size()) + " 条，以下展示前 " +
                  to_string(n) + " 条）:\n";
    for (int i = 0; i < n; i++)
        text += to_string(i + 1) + ". " + results[i] + "\n";
    return text;
}

// 从 JSON 数组里收集条目（兼容数组 / results / RelatedTopics 结构，DuckDuckGo 特判 Topics 嵌套）
static void collectJsonResults(const Json::Value &arr, vector<string> &out)
{
    for (int i = 0; i < (int)arr.size(); i++)
    {
        const Json::Value &it = arr[i];
        if (!it.isObject())
            continue;

        // DuckDuckGo 的 RelatedTopics 里还嵌套了 Topics，跳过只有子列表的项
        if (it.isMember("Topics") && it["Topics"].isArray() && !it.isMember("FirstURL") && !it.isMember("Text"))
            continue;

        string title   = searchField(it, {"title", "Title", "heading", "result"});
        string url     = searchField(it, {"url", "URL", "href", "link", "FirstURL"});
        string snippet = searchField(it, {"content", "snippet", "description", "body", "Text"});
        if (title.empty() && !snippet.empty())
        {
            // DuckDuckGo RelatedTopics.Text 形如 "标题 - 摘要"
            size_t dash = snippet.find(" - ");
            if (dash != string::npos)
            {
                title = snippet.substr(0, dash);
                snippet = snippet.substr(dash + 3);
            }
            else
                title = "(无标题)";
        }
        if (url.empty() && title.empty())
            continue;

        string line;
        if (!title.empty())
            line += title + "\n";
        if (!url.empty())
            line += "  " + url + "\n";
        if (!snippet.empty())
            line += "  " + snippet + "\n";
        if (!line.empty())
            out.push_back(line);
    }
}

JZWebSearchTool::JZWebSearchTool()
{
    ToolDefine def;
    def.name = "web_search";
    def.description = "联网搜索：给定查询词，返回若干条网页搜索结果（标题、链接、摘要）。" \
                      "适合查找官方文档、API 用法、报错原因、第三方库等需要实时信息的问题。";
    def.parameters.push_back(makeParam("query", ParamType::String, "搜索关键词"));
    def.parameters.push_back(makeParam("num_results", ParamType::Number, "返回几条结果，默认 5，最多 20"));
    def.required_parameters.push_back("query");
    setToolDefine(def);

    // 模板默认指向内置 Bing RSS；调用方可 setSearchUrlTemplate 覆盖
    m_searchUrlTemplate = kBingRssSearchUrl;
}

void JZWebSearchTool::setSearchUrlTemplate(const string &urlTemplate)
{
    m_searchUrlTemplate = urlTemplate;
}

void JZWebSearchTool::setNetworkAccessManager(JZNetworkAccessManager *net)
{
    m_net = net;
}

void JZWebSearchTool::call(const ToolCall &params)
{
    string query = str_trimmed(params.params.get("query", "").asString());
    if (query.empty())
    {
        finish(params, false, "参数 query 不能为空");
        return;
    }
    int num = params.params.get("num_results", 5).asInt();
    if (num < 1)
        num = 5;
    if (num > 20)
        num = 20;

    // 组装候选源：配置了自定义模板就只用自定义源；否则用内置默认源（Bing RSS 优先）
    // ⚠ 这些状态放在**本次调用的 ctx** 里，不放成员变量 —— 同一工具会被并行调用(模型一轮里
    //   可能同时发多个查询)，共享成员会互相覆盖(见头文件 SearchCall 的说明)。
    auto ctx = std::make_shared<SearchCall>();
    ctx->ctxId = ++m_ctxIdGen;
    ctx->call = params;
    ctx->query = query;
    ctx->num = num;
    // 模板默认即内置 Bing RSS（构造时初始化），可被 setSearchUrlTemplate 覆盖
    ctx->queue.push_back(m_searchUrlTemplate);
    m_calls[ctx->ctxId] = ctx;

    runNextSearch(ctx);
}

// 取消某次调用：只关掉它自己的在途请求（别的调用/别的源不受影响）
void JZWebSearchTool::cancel(const std::string &id)
{
    for (auto it = m_calls.begin(); it != m_calls.end(); ++it)
    {
        if (it->second->call.id != id)
            continue;
        auto ctx = it->second;
        m_calls.erase(it);
        if (ctx->reply)
        {
            auto reply = ctx->reply;
            ctx->reply = nullptr;
            reply->close();
        }
        return;
    }
    JZCodeTool::cancel(id);
}

void JZWebSearchTool::finishSearch(const std::shared_ptr<SearchCall> &ctx, bool ok, const std::string &text)
{
    finish(ctx->call, ok, text);
    m_calls.erase(ctx->ctxId);   // 本次调用结束, 摘掉上下文
}

void JZWebSearchTool::runNextSearch(const std::shared_ptr<SearchCall> &ctx)
{
    if (ctx->queue.empty())
    {
        string detail = ctx->errors.empty() ? string() : ("\n各源失败详情:\n" + ctx->errors);
        finishSearch(ctx, false, "网络搜索失败: 候选搜索源均不可用。"
               "若当前网络无法直连这些源，请在设置里配置可用的 webSearchUrl 模板（返回 JSON 或 RSS）。" + detail);
        return;
    }

    string url = ctx->queue.front();
    ctx->queue.erase(ctx->queue.begin());
    ctx->currentUrl = url;

    // 展开 {q} 占位符
    string enc = urlEncodeQuery(ctx->query);
    for (;;)
    {
        size_t pos = url.find("{q}");
        if (pos == string::npos)
            break;
        url.replace(pos, 3, enc);
    }

    // 共享管理器必须由调用方注入（setNetworkAccessManager），未注入属于非法使用
    JZ_ASSERT(m_net);

    JZNetworkRequest req(url);
    req.setHeader(JZNetworkRequest::UserAgentHeader, kSearchUserAgent);
    JZNetworkReply *reply = m_net->get(req);
    if (!reply)
    {
        failOverSearch(ctx, ctx->currentUrl + ": 创建网络请求失败");
        return;
    }
    // 本调用独占这条请求：并行调用各有各的 ctx->reply，谁也不取消谁（旧实现是共享 m_reply +
    // "新调用 close 掉上一次"，并发时会把对方的结果吃掉）
    ctx->reply = reply;

    // 单源 15s 超时：超时按失败处理，自动切到下一个候选源
    // 重定向策略必须显式放宽：必应对 www.bing.com 的搜索请求会 **302 到 cn.bing.com(跨主机)**，
    // 默认策略是 SameOrigin(JZNetworkReply 构造)，**不会跟随** ⇒ 拿到的只是 302 的 HTML 占位页，
    // 而 parseSearch 只认 JSON / RSS ⇒ 一律判"返回内容无法识别" ⇒ 队列里只剩这一个源就报
    // "候选搜索源均不可用"。这里与 web_fetch(下面同类位置) 对齐：允许跨主机跳转，上限 5 跳。
    reply->setFollowRedirects(true);
    reply->setRedirectPolicy(JZNetworkReply::AllAllowed);
    reply->setMaxRedirects(5);
    reply->setTimeOut(15000);
    connect(&reply->sigFinished, this, [this, ctx, reply]()
    {
        if (ctx->reply != reply)   // 本次调用已被取消 / 已换到下一个源：忽略这条旧回调
            return;
        ctx->reply = nullptr;
        handleReply(ctx, reply);
    });
}

void JZWebSearchTool::failOverSearch(const std::shared_ptr<SearchCall> &ctx, const string &reason)
{
    if (!ctx->errors.empty())
        ctx->errors += "\n";
    ctx->errors += reason;
    runNextSearch(ctx);
}

void JZWebSearchTool::handleReply(const std::shared_ptr<SearchCall> &ctx, JZNetworkReply *reply)
{
    JZNetworkReply::NetworkError err = reply->error();
    if (err != JZNetworkReply::NoError && err != JZNetworkReply::None)
    {
        failOverSearch(ctx, ctx->currentUrl + ": " + reply->errorString());
        return;
    }
    int code = reply->httpStatusCode();
    if (code < 200 || code >= 300)
    {
        string extra = (code == 0) ? "（无法连接，请检查网络/代理设置）" : string();
        failOverSearch(ctx, ctx->currentUrl + ": HTTP " + to_string(code) + extra);
        return;
    }

    string body = str_fromUtf8(reply->readAll());
    parseSearch(ctx, body);
}

void JZWebSearchTool::parseSearch(const std::shared_ptr<SearchCall> &ctx, const string &body)
{
    vector<string> results;

    // ---------- JSON 源（数组 / results / RelatedTopics） ----------
    string trimmed = str_trimmed(body);
    if (!trimmed.empty() && (trimmed[0] == '{' || trimmed[0] == '['))
    {
        Json::Value root;
        Json::Reader reader;
        if (reader.parse(body, root))
        {
            const Json::Value *arr = nullptr;
            if (root.isArray())
                arr = &root;
            else if (root.isObject())
            {
                if (root.isMember("results") && root["results"].isArray())
                    arr = &root["results"];
                else if (root.isMember("RelatedTopics") && root["RelatedTopics"].isArray())
                    arr = &root["RelatedTopics"];
            }
            if (arr)
            {
                collectJsonResults(*arr, results);
                finishSearch(ctx, true, formatSearchResults(results, ctx->num));
                return;
            }
        }
    }

    // ---------- RSS/Atom 源（必应 format=rss 等） ----------
    if (looksRssFeed(body))
    {
        collectRssResults(body, results);
        finishSearch(ctx, true, formatSearchResults(results, ctx->num));
        return;
    }

    // ---------- 无法识别（可能是登录页 / 验证码页 / 错误页）：回退下一个候选源 ----------
    string snippet = body.size() > 300 ? body.substr(0, 300) : body;
    failOverSearch(ctx, ctx->currentUrl + ": 返回内容无法识别（非 JSON/RSS，前 300 字符: " + snippet + "）");
}

// ==================== JZWebFetchTool ====================
// 把一段响应字节按声明的字符集转成 UTF-8：
//   - utf-8 / 未声明：直接按 UTF-8 解码
//   - 其它（gbk / gb2312 / big5 等）：经 str_local_to_utf8 按系统本地代码页转 UTF-8
//     （中文系统本地代码页为 936，可正确解 gbk；非中文系统为尽力而为）
// 注：本文件包含 JZAgentTool.h（其枚举已改为 enum class ParamType::Boolean，原先与 <winnt.h>
//     的 BOOLEAN 冲突已消除）；这里仍复用 JZStd 的 str_local_to_utf8（windows 调用在独立 TU）。
static string fetchDecode(const JZByteArray &body, const string &charset)
{
    int n = body.size();
    if (n == 0)
        return string();

    string lower = charset;
    for (char &c : lower)
        c = (char)::tolower((unsigned char)c);

    bool utf8 = lower.empty() || lower.find("utf-8") != string::npos ||
                lower.find("utf8") != string::npos;
    if (utf8)
        return str_fromUtf8(JZByteArray(body.data(), n));

    // 非 UTF-8：按系统本地代码页尽力转换（中文系统可正确解 gbk）
    string local((const char *)body.data(), n);
    return str_local_to_utf8(local);
}

// 极简 HTML -> 纯文本：去 script/style、块级标签换换行、去其余标签、解实体、压空白。
static string htmlToText(const string &html)
{
    string s = html;

    // 1. 去掉 <script>...</script> 与 <style>...</style>（大小写不敏感）
    auto dropBlock = [&](const char *tag)
    {
        string open = string("<") + tag;
        string close = string("</") + tag + ">";
        string low = s;
        for (char &c : low) c = (char)::tolower((unsigned char)c);
        size_t pos = 0;
        for (;;)
        {
            size_t i = low.find(open, pos);
            if (i == string::npos) break;
            size_t j = low.find(close, i);
            if (j == string::npos) break;
            size_t end = j + close.size();
            s.erase(i, end - i);
            low.erase(i, end - i);
            pos = i;
        }
    };
    dropBlock("script");
    dropBlock("style");

    // 2. 块级标签后补换行，便于阅读
    static const char *brk[] = {
        "<br", "<p", "</p", "<div", "</div", "<li", "</li",
        "<tr", "</tr", "<h1", "<h2", "<h3", "<h4", "<h5", "<h6",
        "</h", "<table", "</table", "<ul", "</ul", "<ol", "</ol",
        "<section", "<article", "<header", "<footer", "<nav"
    };
    for (const char *b : brk)
    {
        string low = s;
        for (char &c : low) c = (char)::tolower((unsigned char)c);
        size_t p = 0;
        for (;;)
        {
            size_t i = low.find(b, p);
            if (i == string::npos) break;
            s.insert(i, "\n");
            low.insert(i, "\n");
            p = i + (int)strlen(b) + 1;
        }
    }

    // 3. 去所有剩余标签 <...>
    {
        string out;
        bool inTag = false;
        for (char c : s)
        {
            if (c == '<') inTag = true;
            else if (c == '>') inTag = false;
            else if (!inTag) out.push_back(c);
        }
        s = out;
    }

    // 4. 解常用实体
    auto replaceAll = [](string &t, const string &from, const string &to)
    {
        size_t p = 0;
        for (;;)
        {
            size_t i = t.find(from, p);
            if (i == string::npos) break;
            t.replace(i, from.size(), to);
            p = i + to.size();
        }
    };
    replaceAll(s, "&nbsp;", " ");
    replaceAll(s, "&amp;", "&");
    replaceAll(s, "&lt;", "<");
    replaceAll(s, "&gt;", ">");
    replaceAll(s, "&quot;", "\"");
    replaceAll(s, "&#39;", "'");
    replaceAll(s, "&apos;", "'");

    // 数字实体 &#NN; / &#xHH;
    for (;;)
    {
        size_t i = s.find("&#");
        if (i == string::npos) break;
        size_t j = s.find(';', i);
        if (j == string::npos) break;
        string num = s.substr(i + 2, j - i - 2);
        int code = 0;
        if (!num.empty() && (num[0] == 'x' || num[0] == 'X'))
            code = (int)strtol(num.c_str() + 1, nullptr, 16);
        else
            code = (int)strtol(num.c_str(), nullptr, 10);
        if (code > 0 && code < 0x110000)
        {
            char buf[5] = {0, 0, 0, 0, 0};
            if (code < 0x80)
                buf[0] = (char)code;
            else if (code < 0x800)
            {
                buf[0] = (char)(0xC0 | (code >> 6));
                buf[1] = (char)(0x80 | (code & 0x3F));
            }
            else if (code < 0x10000)
            {
                buf[0] = (char)(0xE0 | (code >> 12));
                buf[1] = (char)(0x80 | ((code >> 6) & 0x3F));
                buf[2] = (char)(0x80 | (code & 0x3F));
            }
            else
            {
                buf[0] = (char)(0xF0 | (code >> 18));
                buf[1] = (char)(0x80 | ((code >> 12) & 0x3F));
                buf[2] = (char)(0x80 | ((code >> 6) & 0x3F));
                buf[3] = (char)(0x80 | (code & 0x3F));
            }
            s.replace(i, j - i + 1, buf);
        }
        else
            s.replace(i, j - i + 1, "?");
    }

    // 5. 压空白：连续换行压成最多两个，其它连续空白压成单空格
    string out;
    int nlrun = 0;
    for (char c : s)
    {
        if (c == '\n' || c == '\r')
        {
            nlrun++;
            if (nlrun <= 2) out.push_back('\n');
        }
        else if (c == ' ' || c == '\t')
        {
            nlrun = 0;
            if (!out.empty() && out.back() != ' ' && out.back() != '\n')
                out.push_back(' ');
        }
        else
        {
            nlrun = 0;
            out.push_back(c);
        }
    }
    size_t a = out.find_first_not_of(" \t\r\n");
    if (a == string::npos) return string();
    size_t b = out.find_last_not_of(" \t\r\n");
    return out.substr(a, b - a + 1);
}

JZWebFetchTool::JZWebFetchTool()
{
    ToolDefine def;
    def.name = "web_fetch";
    def.description = "抓取指定网页的正文并以纯文本返回，用于获取搜索结果中某条链接的详细内容、" \
                      "官方文档、API 说明等。返回的是网页转换后的纯文本（已按 max_chars 截断），" \
                      "不是原始 HTML。仅支持 http/https。";
    def.parameters.push_back(makeParam("url", ParamType::String, "要抓取的网页 URL（http/https）"));
    def.parameters.push_back(makeParam("max_chars", ParamType::Number, "返回正文的最大字符数，默认 20000，最多 200000"));
    def.required_parameters.push_back("url");
    setToolDefine(def);
}

void JZWebFetchTool::setNetworkAccessManager(JZNetworkAccessManager *net)
{
    m_net = net;
}

void JZWebFetchTool::call(const ToolCall &params)
{
    string url = str_trimmed(params.params.get("url", "").asString());
    if (url.empty())
    {
        finish(params, false, "参数 url 不能为空");
        return;
    }

    JZUrl u(url);
    if (!u.isValid())
    {
        finish(params, false, "url:\"" + url + "\" 无效");
        return;
    }

    string scheme = u.scheme();
    for (char &c : scheme) c = (char)::tolower((unsigned char)c);
    if (scheme != "http" && scheme != "https")
    {
        finish(params, false, "仅支持 http/https 链接，不支持: " + (scheme.empty() ? "(无协议)" : scheme));
        return;
    }

    int maxChars = params.params.get("max_chars", 20000).asInt();
    if (maxChars < 500) maxChars = 500;
    if (maxChars > 200000) maxChars = 200000;

    // 每次调用一份上下文（并行安全）。⚠ 以前这里是"取消上一次仍在进行中的请求"：
    //   模型一轮里并发抓多个链接时，后一个会把前一个的请求 close 掉 ⇒ 前一个的结果丢了
    //   ⇒ agent 一直等它 ⇒ 整轮停住。现在各调用各有各的 ctx/reply, 谁也不取消谁。
    auto ctx = std::make_shared<FetchCall>();
    ctx->ctxId = ++m_ctxIdGen;
    ctx->call = params;
    ctx->maxChars = maxChars;
    m_calls[ctx->ctxId] = ctx;

    // 共享管理器必须由调用方注入（setNetworkAccessManager），未注入属于非法使用
    JZ_ASSERT(m_net);

    JZNetworkRequest req(url);
    req.setHeader(JZNetworkRequest::UserAgentHeader, "JZCodeAgent/1.0");
    req.setHeader(JZNetworkRequest::AcceptHeader, "text/html,application/xhtml+xml,application/xml,text/plain;q=0.9,*/*;q=0.8");

    JZNetworkReply *reply = m_net->get(req);
    if (!reply)
    {
        finishFetch(ctx, false, "创建网络请求失败");
        return;
    }
    ctx->reply = reply;
    reply->setFollowRedirects(true);
    reply->setRedirectPolicy(JZNetworkReply::AllAllowed);
    reply->setMaxRedirects(5);
    reply->setTimeOut(30000);

    connect(&reply->sigFinished, this, [this, ctx, reply]()
    {
        if (ctx->reply != reply)   // 本次调用已被取消：忽略这条旧回调
            return;
        ctx->reply = nullptr;
        handleReply(ctx, reply);
    });
}

// 取消某次调用：只关掉它自己的在途请求
void JZWebFetchTool::cancel(const std::string &id)
{
    for (auto it = m_calls.begin(); it != m_calls.end(); ++it)
    {
        if (it->second->call.id != id)
            continue;
        auto ctx = it->second;
        m_calls.erase(it);
        if (ctx->reply)
        {
            auto reply = ctx->reply;
            ctx->reply = nullptr;
            reply->close();
        }
        return;
    }
    JZCodeTool::cancel(id);
}

void JZWebFetchTool::finishFetch(const std::shared_ptr<FetchCall> &ctx, bool ok, const std::string &text)
{
    finish(ctx->call, ok, text);
    m_calls.erase(ctx->ctxId);
}

void JZWebFetchTool::handleReply(const std::shared_ptr<FetchCall> &ctx, JZNetworkReply *reply)
{
    JZNetworkReply::NetworkError err = reply->error();
    if (err != JZNetworkReply::NoError && err != JZNetworkReply::None)
    {
        finishFetch(ctx, false, "网页抓取失败: " + reply->errorString());
        return;
    }
    int code = reply->httpStatusCode();
    if (code < 200 || code >= 300)
    {
        finishFetch(ctx, false, "网页返回 HTTP " + to_string(code) +
               (code == 0 ? "（无法连接，请检查网络/代理设置）" : string()));
        return;
    }

    string ctype = reply->header("content-type");
    string mime = ctype;
    size_t sc = mime.find(';');
    if (sc != string::npos)
        mime = mime.substr(0, sc);
    for (char &c : mime) c = (char)::tolower((unsigned char)c);

    if (!mime.empty())
    {
        bool binary = mime.find("image/") == 0 || mime.find("audio/") == 0 ||
                      mime.find("video/") == 0 || mime == "application/pdf" ||
                      mime == "application/zip" || mime == "application/octet-stream" ||
                      mime.find("font/") == 0;
        if (binary)
        {
            finishFetch(ctx, false, "不支持抓取该类型的资源: " + mime);
            return;
        }
    }

    JZByteArray raw = reply->readAll();
    const int kMaxRaw = 5 * 1024 * 1024;   // 5MB 上限，避免大文件撑爆内存
    if (raw.size() > kMaxRaw)
        raw = raw.left(kMaxRaw);

    // 取 charset（去除首尾引号）
    string charset;
    size_t cpos = ctype.find("charset=");
    if (cpos != string::npos)
        charset = ctype.substr(cpos + 8);
    charset = str_trimmed(charset);
    if (!charset.empty() && (charset.front() == '"' || charset.front() == '\''))
        charset = charset.substr(1);
    if (!charset.empty() && (charset.back() == '"' || charset.back() == '\''))
        charset.pop_back();

    string text = fetchDecode(raw, charset);

    // 仅对 html/xml 做标签清洗；纯文本/json 等原样返回
    bool isHtml = mime.find("html") != string::npos ||
                  mime.find("xml") != string::npos ||
                  text.find("<!doctype") != string::npos ||
                  text.find("<html") != string::npos;
    if (isHtml)
        text = htmlToText(text);

    if ((int)text.size() > ctx->maxChars)
        text = text.substr(0, (size_t)ctx->maxChars);

    if (text.empty())
    {
        finishFetch(ctx, true, "页面未解析出可读文本内容（可能是纯脚本页面或需要登录）。");
        return;
    }

    string header = "网页正文（来自 " + ctx->call.params.get("url", "").asString() +
                    "，约 " + to_string(text.size()) + " 字符）:\n";
    finishFetch(ctx, true, header + text);
}

