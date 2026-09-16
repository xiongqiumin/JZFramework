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

    string res = prefix + "/";
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
static bool isValidUtf8(const char *data, int size)
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
            return false;                // 多字节序列被截断
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
    return isValidUtf8(data, head.size());
}

static string truncateMiddle(const string &text, int maxChars)
{
    if ((int)text.size() <= maxChars || maxChars <= 0)
        return text;

    int keep = maxChars / 2;
    int omit = (int)text.size() - keep * 2;
    return text.substr(0, keep) + "\n... 省略 " + to_string(omit) + " 字符 ...\n" + text.substr(text.size() - keep);
}

// 组装真正要启动的进程（program + 参数列表）
//   统一交给系统 shell 执行：Windows 用 cmd /c，Linux 用 sh -c。
//   这样 dir/echo 等内部命令、管道(|)、重定向(>)、&& 串联、.bat/.cmd 都能正常工作。
//   Windows 下取 ComSpec 的绝对路径，避免 CreateProcess 走 PATH/当前目录查找
//   把工作区里同名的 cmd.exe 拉起来。
static bool buildProcessArgs(const string &command, string &program, vector<string> &args)
{
    args.clear();
#ifdef _WIN32
    // 取 ComSpec 的绝对路径：避免 CreateProcess 走 PATH/当前目录查找。
    // (MSVC 下 getenv 会报 C4996，用 _dupenv_s 代替)
    string comspec;
#ifdef _MSC_VER
    char *envBuf = nullptr;
    size_t envLen = 0;
    if (_dupenv_s(&envBuf, &envLen, "ComSpec") == 0 && envBuf)
    {
        comspec = envBuf;
        free(envBuf);
    }
#else
    const char *envBuf = getenv("ComSpec");
    if (envBuf)
        comspec = envBuf;
#endif
    program = comspec.empty() ? string("cmd.exe") : comspec;
    args.push_back("/c");
#else
    program = "/bin/sh";
    args.push_back("-c");
#endif
    args.push_back(command);
    return true;
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

const char *denyReasonText(DenyReason reason)
{
    switch (reason)
    {
    case DenyReason::PathOutside:   return "路径越界";
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

    auto &cfg = m_host->toolZone();
    auto path = fullPath(rel_path);
    if (str_startsWith(path, cfg.workspace))
        return DenyReason::None;

    for (size_t i = 0; i < cfg.trustedDirs.size(); i++)
    {
        if (str_startsWith(path, cfg.trustedDirs[i].path))
        {
            if (op == ToolOperator::Read)
                return DenyReason::None;
            else if (op == ToolOperator::Write && !cfg.trustedDirs[i].writable)
                return DenyReason::WriteDenied;
        }
    }

    return DenyReason::CommandDenied;
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

std::string JZCodeTool::fullPath(std::string path)
{
    if (JZDir::isAbsolutePath(path))
        return JZFileInfo::weaklyCanonicalPath(path);

    string root;
    if (m_host)
        root = m_host->toolZone().workspace;
    if (root.empty())
        return JZFileInfo::weaklyCanonicalPath(path);   // 无宿主: 按进程当前目录理解
    return JZFileInfo::weaklyCanonicalPath(root + "/" + path);
}


// ==================== JZSearchFileTool ====================
JZSearchFileTool::JZSearchFileTool()
{
    ToolDefine def;
    def.name = "search_file";
    def.description = "在工作区的代码中搜索文本或正则表达式，返回 \"相对路径:行号:内容\" 形式的匹配列表。" \
                      "默认跳过隐藏目录和 build/out 等生成目录。";
    def.parameters.push_back(makeParam("pattern", ParamType::String, "要搜索的文本或正则表达式"));
    def.parameters.push_back(makeParam("path", ParamType::String, "在哪个目录下搜索，默认 \".\" 表示整个工作区"));
    def.parameters.push_back(makeParam("file_filter", ParamType::String, "文件名过滤，支持 * ? 与逗号分隔多个模式，如 \"*.cpp,*.h\"，默认 \"*\""));
    def.parameters.push_back(makeParam("case_sensitive", ParamType::Boolean, "是否区分大小写，默认 false"));
    def.parameters.push_back(makeParam("regex", ParamType::Boolean, "pattern 是否按正则表达式解析，默认 false（纯文本搜索）"));
    def.parameters.push_back(makeParam("max_results", ParamType::Number, "最多返回多少条匹配，默认 50"));
    def.parameters.push_back(makeParam("max_files", ParamType::Number, "最多扫描多少个文件，默认 1000，避免大仓库一次扫太多"));
    def.required_parameters.push_back("pattern");
    setToolDefine(def);
}

static void searchInDir(const string &absDir, const string &relBase, const string &pattern,
                        const string &fileFilter, bool caseSensitive, bool useRegex,
                        int maxResults, int maxFiles,                         long long maxBytes,
                        vector<string> &out, int &totalFiles, long long &totalBytes, bool &stopped,
                        ICodeToolHost *host)
{
    if (stopped || (int)out.size() >= maxResults || totalFiles >= maxFiles || totalBytes >= maxBytes)
        return;

    JZDir dir(absDir);
    vector<JZFileInfo> list = dir.entryInfoList(JZDir::Files | JZDir::Dirs | JZDir::NoDotAndDotDot, JZDir::SortName);
    for (const auto &info : list)
    {
        if (stopped || (int)out.size() >= maxResults || totalFiles >= maxFiles || totalBytes >= maxBytes)
            return;

        string rel = relBase.empty() ? info.fileName() : relBase + "/" + info.fileName();
        if (info.isDir())
        {
            if (!isSkipDir(info.fileName()))
                searchInDir(info.filePath(), rel, pattern, fileFilter, caseSensitive, useRegex,
                            maxResults, maxFiles, maxBytes, out, totalFiles, totalBytes, stopped, host);
            continue;
        }

        if (info.size() > 2 * 1024 * 1024)
            continue;
        if (!fileFilterMatch(info.fileName(), fileFilter, false))
            continue;
        if (!isTextFile(info.filePath()))
            continue;

        totalFiles++;
        string content, readError;
        if (!readHostFile(host, info.filePath(), content, readError))
            continue;

        totalBytes += (long long)content.size();
        vector<string> lines = readLines(content);
        JZRegExp reg;
        bool regOk = false;
        if (useRegex)
        {
            int flags = JZRegExp::Global | (caseSensitive ? 0 : JZRegExp::IgnoreCase);
            regOk = reg.compile(JZString::fromStdString(pattern), flags);
        }

        string needle = caseSensitive ? pattern : str_toLower(pattern);
        for (int i = 0; i < (int)lines.size(); i++)
        {
            bool hit = false;
            if (useRegex)
            {
                hit = regOk && reg.test(JZString::fromStdString(lines[i]));
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

    // 因文件数 / 字节数触顶而提前结束
    if (totalFiles >= maxFiles || totalBytes >= maxBytes)
        stopped = true;
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
        finish(params, false, "路径不存在: " + rel, ret);
        return;
    }

    vector<string> results;
    int totalFiles = 0;
    long long totalBytes = 0;
    bool stopped = false;
    if (info.isDir())
    {
        searchInDir(abs, string(), pattern, fileFilter, caseSensitive, useRegex,
                    maxResults, maxFiles, maxBytes, results, totalFiles, totalBytes, stopped,
                    m_host);
    }
    else
    {
        if (!isTextFile(abs))
        {
            finish(params, false, "二进制文件无法搜索: " + rel);
            return;
        }
        string content, readError;
        if (!readHostFile(m_host, abs, content, readError))
        {
            finish(params, false, readError);
            return;
        }
        totalFiles = 1;
        string needle = caseSensitive ? pattern : str_toLower(pattern);
        vector<string> lines = readLines(content);
        for (int i = 0; i < (int)lines.size(); i++)
        {
            string target = caseSensitive ? lines[i] : str_toLower(lines[i]);
            if (str_indexOf(target, needle) != -1)
                results.push_back(rel + ":" + to_string(i + 1) + ": " + lines[i]);
        }
    }

    string text;
    if (results.empty())
    {
        text = "没有找到匹配 \"" + pattern + "\" 的内容（已搜索 " + to_string(totalFiles) + " 个文件）。";
    }
    else
    {
        text = "匹配 \"" + pattern + "\" 共 " + to_string(results.size()) + " 处" +
               (results.size() == (size_t)maxResults ? "（已达结果上限）" : "") + ":\n";
        for (const auto &r : results)
            text += r + "\n";
        if (stopped)
            text += "\n（已扫描 " + to_string(totalFiles) + " 个文件 / 约 " +
                    to_string(totalBytes / (1024 * 1024)) + "MB，达到单次搜索上限，提前停止；"
                    "可缩小 path / file_filter 范围后重试）";
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
    if (!info.exists() || !info.isFile())
    {
        finish(params, false, "文件不存在: " + rel, DenyReason::NotFound);
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

    string dir = jzDirPart(abs);
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
    if (!info.exists())
    {
        finish(params, false, "文件不存在: " + rel + "  (新建文件请用 write_file)",
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

// ==================== JZProcessRunner ====================
JZProcessRunner::~JZProcessRunner()
{
    auto tmp_tasks = m_tasks;
    for (auto& task : tmp_tasks)
        cancel(task.id);
}

bool JZProcessRunner::runProcess(const ToolCall &call, const string &program,
                                 const vector<string> &args, const string &workDir,
                                 int timeoutMs, const string &logText, bool emitCallLog)
{
    JZProcess *proc = new JZProcess(this);
    proc->setWorkingDirectory(workDir);

    string call_id = call.id;
    connect(&proc->sigFinished, this,
            [this, call_id, proc](int code, JZProcess::ExitStatus status)
            {
                onProcFinished(proc, call_id, code, status);
            });

    if (!proc->start(program, args))
    {
        proc->deleteLater();
        finish(call, false, "启动进程失败: " + logText + "\n请确认程序存在且可运行。",
               DenyReason::NotFound);
        return false;
    }

    if (timeoutMs < 1000)
        timeoutMs = 1000;

    JZTimer *timer = new JZTimer(this);
    connect(&timer->sigTimeout, this, [this, proc, timer]()
            {
                for (auto &t : m_tasks)
                {
                    if (t.proc == proc)
                        t.timeout = true;
                }
                timer->stop();
                proc->killProcess();
            });
    timer->start(timeoutMs);

    Task task;
    task.id = call.id;
    task.proc = proc;
    task.timer = timer;
    task.startTime = JZPlatform::getTickCountMs();
    m_tasks.push_back(task);
    return true;
}

void JZProcessRunner::cancel(const std::string& id)
{
    for (size_t i = 0; i < m_tasks.size(); i++)
    {
        if (m_tasks[i].id == id)
        {
            m_tasks[i].proc->killProcess();
            return;
        }
    }
}

void JZProcessRunner::onProcFinished(JZProcess *proc, const string &id,
                                     int exitCode, JZProcess::ExitStatus status)
{
    int64_t costMs = 0;
    bool timeout = false;
    for (const auto &t : m_tasks)
    {
        if (t.proc == proc)
        {
            costMs = JZPlatform::getTickCountMs() - t.startTime;
            timeout = t.timeout;
            break;
        }
    }
    removeTask(proc);

    string out = str_fromLocal(proc->readAllStandardOutput());
    string err = str_fromLocal(proc->readAllStandardError());
    proc->deleteLater();

    bool ok = (status == JZProcess::NormalExit) && (exitCode == 0) && !timeout;

    string text = "退出码: " + to_string(exitCode);
    text += "  耗时: " + to_string(costMs / 1000.0) + "s";
    if (timeout)
        text += "  (超时被终止)";
    text += "\n";

    const int kMaxChars = 12000;
    if (!out.empty())
        text += "[stdout]\n" + truncateMiddle(str_trimmed(out), kMaxChars) + "\n";
    if (!err.empty())
        text += "[stderr]\n" + truncateMiddle(str_trimmed(err), kMaxChars) + "\n";
    if (out.empty() && err.empty())
        text += "(无输出)\n";

    ToolCallResult result;
    result.id = id;
    result.isOk = ok;
    result.result = text;
    sigResult.emit(result);
}

void JZProcessRunner::removeTask(JZProcess *proc)
{
    for (auto it = m_tasks.begin(); it != m_tasks.end(); ++it)
    {
        if (it->proc == proc)
        {
            if (it->timer)
            {
                it->timer->stop();
                it->timer->deleteLater();
            }
            m_tasks.erase(it);
            return;
        }
    }
}

//JZShellTool
JZShellTool::JZShellTool()
{
    ToolDefine def;
    def.name = "command";
    def.description = "执行外部命令。传入完整命令字符串cmd；指定工作目录work_dir, 使用 cmd /c \"command\" 执行";
    def.parameters.push_back(makeParam("cmd", ParamType::String, "要执行的命令字符串"));
    def.parameters.push_back(makeParam("work_dir", ParamType::String, "执行的工作目录，相对工作区路径"));
    def.required_parameters.push_back("cmd");
    setToolDefine(def);
}

void JZShellTool::setTimeOut(int timeout)
{
    m_timeoutMs = timeout;
}

void JZShellTool::setExeSearchPaths(const std::vector<std::string> &paths)
{
    m_exeSearchPaths = paths;
}

void JZShellTool::call(const ToolCall& params)
{
    std::string cmdRaw = params.params.get("cmd", "").asString();
    std::string relDir = params.params.get("work_dir", "").asString();

    if (cmdRaw.empty())
    {
        finish(params, false, "参数 cmd 不能为空");
        return;
    }

    std::string workDir = fullPath(relDir);

    // 工作目录读权限校验（同JZCommandTool）
    DenyReason ret = checkPathPermission(ToolOperator::Read, workDir);
    if (ret != DenyReason::None)
    {
        finish(params, false, denyReasonText(ret), ret);
        return;
    }

    // 额外 exe 搜索路径：注入到 shell 的 PATH 最前面，使按名调用的程序可被找到。
    // 用 shell 自身的 set/export 实现，无需改动 JZProcess 的环境块构造，
    // 且对内部命令(echo/dir)、管道、重定向、&& 串联等全部透明。
    // 仅修改真正下发的命令；展示给用户的 logText 仍用原始 cmdRaw。
    std::string cmd = cmdRaw;
    if (!m_exeSearchPaths.empty())
    {
        std::string joined;
        for (const auto &p : m_exeSearchPaths)
        {
            std::string np = codeNormalizePath(p);
#ifdef _WIN32
            str_replace(np, "/", "\\");
#endif
            if (!np.empty())
#ifdef _WIN32
                joined += (joined.empty() ? std::string() : std::string(";")) + np;
#else
                joined += (joined.empty() ? std::string() : std::string(":")) + np;
#endif
        }
        if (!joined.empty())
        {
#ifdef _WIN32
            cmd = "set \"PATH=" + joined + ";%PATH%\" && " + cmdRaw;
#else
            cmd = "export PATH=\"" + joined + ":$PATH\"; " + cmdRaw;
#endif
        }
    }

    std::string program;
    std::vector<std::string> args;
    if (!buildProcessArgs(cmd, program, args))
    {
        finish(params, false, "命令解析失败: " + cmdRaw);
        return;
    }

    int timeout = m_timeoutMs;
    // runProcess 签名：(params, program, args, workDir, timeout, displayText)
    runProcess(params, program, args, workDir, timeout, cmdRaw);
}

// ==================== JZCommandTool ====================
JZCommandTool::JZCommandTool()
{
}

void JZCommandTool::setup(const std::string& name, const std::string& desc,
    const std::string& command, bool has_args, const std::string& work_dir)
{
    m_command = command;

    ToolDefine def;
    def.name = name;
    def.description = desc;
    // has_args 才开放 args 参数：未开放的固定命令工具不接受模型追加参数
    if (has_args)
    {
        def.parameters.push_back(makeParam("args", ParamType::String, "命令参数"));
        m_hasArgs = true;
    }

    if(work_dir.empty())
        def.parameters.push_back(makeParam("work_dir", ParamType::String, "工作目录"));
    else
        m_defaultWorkDir = work_dir;

    setToolDefine(def);
}

void JZCommandTool::call(const ToolCall &params)
{
    string workDir;
    if (!m_defaultWorkDir.empty())
    {
        workDir = m_defaultWorkDir;
    }
    else
    {
        string relDir = params.params.get("work_dir", "").asString();
        workDir = fullPath(relDir);
    }
    
    DenyReason ret = checkPathPermission(ToolOperator::Read, workDir);
    if (ret != DenyReason::None)
    {
        finish(params, false, denyReasonText(ret), ret);
        return;
    }

    // 拼接参数：args=true 时把模型传的 args 追加到命令之后
    string cmd = m_command;
    if (m_hasArgs)
    {
        string extra = str_trimmed(params.params.get("args", "").asString());
        if (!extra.empty())
            cmd = cmd + " " + extra;
    }
    int timeout = m_timeoutMs;

    string program;
    vector<string> args;
    if (!buildProcessArgs(cmd, program, args))
    {
        finish(params, false, "命令解析结果为空: " + cmd);
        return;
    }

    runProcess(params, program, args, workDir, timeout, cmd);
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
        finish(call, true, "编译通过，现在可以用 run_js 运行程序。");
    else
        finish(call, false, error);
}

// ==================== 虚拟终端：区域模型 ====================
// 根目录统一规范化：统一分隔符、消掉 "." / ".."、去掉尾部斜杠
// （保留 "C:/" 这样的盘符根，size()>2 的判断就是为此）
static string zoneNormalizeRoot(const string &path)
{
    string r = codeNormalizePath(path);
    while (r.size() > 2 && str_endsWith(r, "/"))
        str_chop(r, 1);
    return r;
}

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
    else if (codeIsAbsPath(p))
        full = p;
    else
        full = base + "/" + p;

    full = codeNormalizePath(full);
    Zone z = zoneOfPath(cfg, full);
    if (z == Zone::Outside)
    {
        error = "路径越界，只允许访问工作区与授信目录: " + input;
        return false;
    }

    absPath = full;
    zone = z;
    return true;
}

bool zoneCheckWrite(const ZoneConfig &cfg, const string &absPath, string &error)
{
    Zone z = zoneOfPath(cfg, absPath);
    if (z == Zone::Workspace)
        return true;
    if (z == Zone::Trusted)
    {
        // 只有被显式标记为可写的授信目录才允许写入
        string p = codeNormalizePath(absPath);
        for (const auto &d : cfg.trustedDirs)
        {
            if (d.writable && codeIsSubPath(p, zoneNormalizeRoot(d.path)))
            {
                error.clear();
                return true;
            }
        }
        error = "写操作只允许发生在工作区，或被标记为可写的授信目录: " + absPath;
        return false;
    }
    error = "写操作只允许发生在工作区: " + absPath;
    return false;
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
    m_searchQuery = query;
    m_searchNum = num;
    m_searchErrors.clear();
    m_searchQueue.clear();
    // 模板默认即内置 Bing RSS（构造时初始化），可被 setSearchUrlTemplate 覆盖
    m_searchQueue.push_back(m_searchUrlTemplate);

    runNextSearch(params);
}

void JZWebSearchTool::runNextSearch(const ToolCall &call)
{
    if (m_searchQueue.empty())
    {
        string detail = m_searchErrors.empty() ? string() : ("\n各源失败详情:\n" + m_searchErrors);
        finish(call, false, "网络搜索失败: 候选搜索源均不可用。"
               "若当前网络无法直连这些源，请在设置里配置可用的 webSearchUrl 模板（返回 JSON 或 RSS）。" + detail);
        return;
    }

    string url = m_searchQueue.front();
    m_searchQueue.erase(m_searchQueue.begin());
    m_lastSearchUrl = url;

    // 取消上一次仍在进行中的请求（其回调稍后到达时，会因 m_reply 已被替换而被忽略）
    if (m_reply)
    {
        auto old = m_reply;
        m_reply = nullptr;
        old->close();
    }

    // 展开 {q} 占位符
    string enc = urlEncodeQuery(m_searchQuery);
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
    m_reply = m_net->get(req);
    if (!m_reply)
    {
        failOverSearch(call, m_lastSearchUrl + ": 创建网络请求失败");
        return;
    }

    // 单源 15s 超时：超时按失败处理，自动切到下一个候选源
    JZNetworkReply *reply = m_reply;
    reply->setTimeOut(15000);
    connect(&reply->sigFinished, this, [this, reply, call]()
    {
        if (m_reply != reply)   // 已被新一轮 call 或取消请求取代，忽略旧回调
            return;
        m_reply = nullptr;
        handleReply(reply, call);
    });
}

void JZWebSearchTool::failOverSearch(const ToolCall &call, const string &reason)
{
    if (!m_searchErrors.empty())
        m_searchErrors += "\n";
    m_searchErrors += reason;
    runNextSearch(call);
}

void JZWebSearchTool::handleReply(JZNetworkReply *reply, const ToolCall &call)
{
    JZNetworkReply::NetworkError err = reply->error();
    if (err != JZNetworkReply::NoError && err != JZNetworkReply::None)
    {
        failOverSearch(call, m_lastSearchUrl + ": " + reply->errorString());
        return;
    }
    int code = reply->httpStatusCode();
    if (code < 200 || code >= 300)
    {
        string extra = (code == 0) ? "（无法连接，请检查网络/代理设置）" : string();
        failOverSearch(call, m_lastSearchUrl + ": HTTP " + to_string(code) + extra);
        return;
    }

    string body = str_fromUtf8(reply->readAll());
    parseSearch(body, call);
}

void JZWebSearchTool::parseSearch(const string &body, const ToolCall &call)
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
                finish(call, true, formatSearchResults(results, m_searchNum));
                return;
            }
        }
    }

    // ---------- RSS/Atom 源（必应 format=rss 等） ----------
    if (looksRssFeed(body))
    {
        collectRssResults(body, results);
        finish(call, true, formatSearchResults(results, m_searchNum));
        return;
    }

    // ---------- 无法识别（可能是登录页 / 验证码页 / 错误页）：回退下一个候选源 ----------
    string snippet = body.size() > 300 ? body.substr(0, 300) : body;
    failOverSearch(call, m_lastSearchUrl + ": 返回内容无法识别（非 JSON/RSS，前 300 字符: " + snippet + "）");
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

    // 取消上一次仍在进行中的请求
    if (m_reply)
    {
        m_reply->close();
        m_reply = nullptr;
    }

    // 共享管理器必须由调用方注入（setNetworkAccessManager），未注入属于非法使用
    JZ_ASSERT(m_net);

    JZNetworkRequest req(url);
    req.setHeader(JZNetworkRequest::UserAgentHeader, "JZCodeAgent/1.0");
    req.setHeader(JZNetworkRequest::AcceptHeader, "text/html,application/xhtml+xml,application/xml,text/plain;q=0.9,*/*;q=0.8");

    m_reply = m_net->get(req);
    if (!m_reply)
    {
        finish(params, false, "创建网络请求失败");
        return;
    }
    m_reply->setFollowRedirects(true);
    m_reply->setRedirectPolicy(JZNetworkReply::AllAllowed);
    m_reply->setMaxRedirects(5);
    m_reply->setTimeOut(30000);

    JZNetworkReply *reply = m_reply;
    connect(&reply->sigFinished, this, [this, reply, params, maxChars]()
    {
        if (m_reply == reply)
            m_reply = nullptr;
        handleReply(reply, params, maxChars);
    });
}

void JZWebFetchTool::handleReply(JZNetworkReply *reply, const ToolCall &call, int maxChars)
{
    JZNetworkReply::NetworkError err = reply->error();
    if (err != JZNetworkReply::NoError && err != JZNetworkReply::None)
    {
        finish(call, false, "网页抓取失败: " + reply->errorString());
        return;
    }
    int code = reply->httpStatusCode();
    if (code < 200 || code >= 300)
    {
        finish(call, false, "网页返回 HTTP " + to_string(code) +
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
            finish(call, false, "不支持抓取该类型的资源: " + mime);
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

    if ((int)text.size() > maxChars)
        text = text.substr(0, (size_t)maxChars);

    if (text.empty())
    {
        finish(call, true, "页面未解析出可读文本内容（可能是纯脚本页面或需要登录）。");
        return;
    }

    string header = "网页正文（来自 " + call.params.get("url", "").asString() +
                    "，约 " + to_string(text.size()) + " 字符）:\n";
    finish(call, true, header + text);
}

