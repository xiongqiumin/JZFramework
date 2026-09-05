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

// 文本文件判断: 前 4K 内出现 '\0' 视为二进制
static bool isTextFile(const string &path)
{
    JZFile file;
    if (!file.open(path, "rb"))
        return false;

    JZByteArray head = file.read(4096);
    file.close();
    const char *data = head.data();
    for (int i = 0; i < head.size(); i++)
    {
        if (data[i] == '\0')
            return false;
    }
    return true;
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

static string fileToString(const string &path, string &error)
{
    JZFile file;
    if (!file.open(path, "rb"))
    {
        error = "打开文件失败: " + path;
        return string();
    }
    JZByteArray data = file.readAll();
    file.close();
    return str_fromUtf8(data);
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

bool codeReadFile(const string &workspace, const string &rel, string &text, string &error)
{
    string abs;
    if (!codeResolvePath(workspace, rel, abs, error))
        return false;

    JZFileInfo info(abs);
    if (!info.exists() || !info.isFile())
    {
        error = "文件不存在: " + rel;
        return false;
    }
    if (info.size() > 2 * 1024 * 1024)
    {
        error = "文件过大(>2MB): " + rel;
        return false;
    }
    if (!isTextFile(abs))
    {
        error = "二进制文件无法预览: " + rel;
        return false;
    }

    text = fileToString(abs, error);
    return !text.empty() || error.empty();
}

const char *denyReasonText(DenyReason reason)
{
    switch (reason)
    {
    case DenyReason::PathOutside:   return "路径越界";
    case DenyReason::NotFound:      return "目标不存在";
    case DenyReason::CommandDenied: return "命令被禁止";
    case DenyReason::ProgramDenied: return "程序不在允许区域";
    case DenyReason::EnvChange:     return "会修改环境或下载安装";
    case DenyReason::EditFailed:    return "编辑失败";
    default:                        return "调用失败";
    }
}

// ==================== JZCodeTool ====================
JZCodeTool::JZCodeTool()
    : sigToolLog(this)
{
}

JZCodeTool::~JZCodeTool()
{
}

void JZCodeTool::setWorkspace(const string &path)
{
    m_workspace = path;
}

const string &JZCodeTool::workspace() const
{
    return m_workspace;
}

bool JZCodeTool::resolvePath(const string &input, string &absPath, string &error) const
{
    return codeResolvePath(m_workspace, input, absPath, error);
}

ParamSchema JZCodeTool::makeParam(const string &name, ParamType type, const string &desc)
{
    ParamSchema p;
    p.name = name;
    p.type = type;
    p.description = desc;
    return p;
}

void JZCodeTool::emitLog(const string &type, const string &text, DenyReason deny)
{
    sigToolLog.emit(type, m_toolDefine.name, text, (int)deny);
}

void JZCodeTool::finish(const ToolCall &call, bool ok, const string &text, DenyReason deny)
{
    emitLog("result", text, deny);

    ToolCallResult result;
    result.id = call.id;
    result.isOk = ok;
    result.result = text;
    sigResult.emit(result);
}

// ==================== JZListDirTool ====================
JZListDirTool::JZListDirTool()
{
    ToolDefine def;
    def.name = "list_dir";
    def.description = "列出工作区中某个目录的文件和子目录。路径使用相对于工作区根目录的相对路径，" \
                      "根目录用 \".\" 表示。返回值中目录名以 \"/\" 结尾。";
    def.parameters.push_back(makeParam("path", STRING, "相对工作区的目录路径，默认 \".\""));
    def.parameters.push_back(makeParam("recursive", BOOLEAN, "是否递归列出子目录，默认 false"));
    def.parameters.push_back(makeParam("max_entries", NUMBER, "最多返回多少条，默认 300"));
    def.required_parameters.push_back("path");
    setToolDefine(def);
}

static void collectDir(const string &absDir, const string &relBase, bool recursive,
                       vector<string> &out, int maxEntries)
{
    JZDir dir(absDir);
    vector<JZFileInfo> list = dir.entryInfoList(JZDir::Files | JZDir::Dirs | JZDir::NoDotAndDotDot, JZDir::SortName);
    for (const auto &info : list)
    {
        if ((int)out.size() >= maxEntries)
            return;

        string rel = relBase.empty() ? info.fileName() : relBase + "/" + info.fileName();
        if (info.isDir())
        {
            out.push_back(rel + "/");
            if (recursive && !isSkipDir(info.fileName()))
                collectDir(info.filePath(), rel, true, out, maxEntries);
        }
        else
        {
            out.push_back(rel);
        }
    }
}

void JZListDirTool::call(const ToolCall &params)
{
    string rel = params.params.get("path", ".").asString();
    bool recursive = params.params.get("recursive", false).asBool();
    int maxEntries = params.params.get("max_entries", 300).asInt();
    if (maxEntries < 1)
        maxEntries = 300;

    string abs, error;
    if (!resolvePath(rel, abs, error))
    {
        finish(params, false, error, DenyReason::PathOutside);
        return;
    }

    JZFileInfo info(abs);
    if (!info.exists())
    {
        finish(params, false, "路径不存在: " + rel, DenyReason::NotFound);
        return;
    }

    emitLog("call", rel);

    vector<string> entries;
    if (info.isDir())
    {
        collectDir(abs, string(), recursive, entries, maxEntries);

        string text = "目录 " + rel + ":\n";
        for (const auto &e : entries)
            text += "  " + e + "\n";
        if ((int)entries.size() >= maxEntries)
            text += "  ... 已达到 max_entries 上限\n";
        finish(params, true, text);
    }
    else
    {
        finish(params, true, rel + "  是文件 (" + to_string(info.size()) + " 字节)，如需查看内容请用 read_file。");
    }
}

// ==================== JZReadFileTool ====================
JZReadFileTool::JZReadFileTool()
{
    ToolDefine def;
    def.name = "read_file";
    def.description = "读取工作区中某个文本文件的内容，带行号返回。可以用 start_line / line_count " \
                      "只读取感兴趣的行范围（行号从 1 开始）。";
    def.parameters.push_back(makeParam("path", STRING, "相对工作区的文件路径"));
    def.parameters.push_back(makeParam("start_line", NUMBER, "起始行号（从 1 开始），默认 1"));
    def.parameters.push_back(makeParam("line_count", NUMBER, "读取多少行，0 或省略表示读到文件末尾"));
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

    string abs, error;
    if (!resolvePath(rel, abs, error))
    {
        finish(params, false, error, DenyReason::PathOutside);
        return;
    }

    emitLog("call", rel);

    JZFileInfo info(abs);
    if (!info.exists() || !info.isFile())
    {
        finish(params, false, "文件不存在: " + rel, DenyReason::NotFound);
        return;
    }
    if (!isTextFile(abs))
    {
        finish(params, false, "不是文本文件: " + rel);
        return;
    }

    string content = fileToString(abs, error);
    if (!error.empty())
    {
        finish(params, false, error);
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

// ==================== JZSearchCodeTool ====================
JZSearchCodeTool::JZSearchCodeTool()
{
    ToolDefine def;
    def.name = "search_code";
    def.description = "在工作区的代码中搜索文本或正则表达式，返回 \"相对路径:行号:内容\" 形式的匹配列表。" \
                      "默认跳过隐藏目录和 build/out 等生成目录。";
    def.parameters.push_back(makeParam("pattern", STRING, "要搜索的文本或正则表达式"));
    def.parameters.push_back(makeParam("path", STRING, "在哪个目录下搜索，默认 \".\" 表示整个工作区"));
    def.parameters.push_back(makeParam("file_filter", STRING, "文件名过滤，支持 * ? 与逗号分隔多个模式，如 \"*.cpp,*.h\"，默认 \"*\""));
    def.parameters.push_back(makeParam("case_sensitive", BOOLEAN, "是否区分大小写，默认 false"));
    def.parameters.push_back(makeParam("regex", BOOLEAN, "pattern 是否按正则表达式解析，默认 false（纯文本搜索）"));
    def.parameters.push_back(makeParam("max_results", NUMBER, "最多返回多少条匹配，默认 50"));
    def.required_parameters.push_back("pattern");
    setToolDefine(def);
}

static void searchInDir(const string &absDir, const string &relBase, const string &pattern,
                        const string &fileFilter, bool caseSensitive, bool useRegex,
                        int maxResults, vector<string> &out, int &totalFiles)
{
    if ((int)out.size() >= maxResults)
        return;

    JZDir dir(absDir);
    vector<JZFileInfo> list = dir.entryInfoList(JZDir::Files | JZDir::Dirs | JZDir::NoDotAndDotDot, JZDir::SortName);
    for (const auto &info : list)
    {
        if ((int)out.size() >= maxResults)
            return;

        string rel = relBase.empty() ? info.fileName() : relBase + "/" + info.fileName();
        if (info.isDir())
        {
            if (!isSkipDir(info.fileName()))
                searchInDir(info.filePath(), rel, pattern, fileFilter, caseSensitive, useRegex, maxResults, out, totalFiles);
            continue;
        }

        if (info.size() > 2 * 1024 * 1024)
            continue;
        if (!fileFilterMatch(info.fileName(), fileFilter, false))
            continue;
        if (!isTextFile(info.filePath()))
            continue;

        totalFiles++;
        string error;
        string content = fileToString(info.filePath(), error);
        if (!error.empty())
            continue;

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
}

void JZSearchCodeTool::call(const ToolCall &params)
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

    string abs, error;
    if (!resolvePath(rel, abs, error))
    {
        finish(params, false, error, DenyReason::PathOutside);
        return;
    }

    JZFileInfo info(abs);
    if (!info.exists())
    {
        finish(params, false, "路径不存在: " + rel, DenyReason::NotFound);
        return;
    }

    emitLog("call", pattern + "  (范围: " + rel + ")");

    vector<string> results;
    int totalFiles = 0;
    if (info.isDir())
    {
        searchInDir(abs, string(), pattern, fileFilter, caseSensitive, useRegex, maxResults, results, totalFiles);
    }
    else
    {
        string content = fileToString(abs, error);
        if (!error.empty())
        {
            finish(params, false, error);
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
               (results.size() == (size_t)maxResults ? "（已达上限）" : "") + ":\n";
        for (const auto &r : results)
            text += r + "\n";
    }
    finish(params, true, text);
}

// ==================== JZModifyCodeTool ====================
JZModifyCodeTool::JZModifyCodeTool()
{
    ToolDefine def;
    def.name = "modify_code";
    def.description = "修改或创建工作区中的文件。"
                      "old_str 为空字符串时，用 new_str 覆盖整个文件（用于新建文件）；"
                      "old_str 非空时，把文件中的 old_str 精确替换为 new_str，默认要求 old_str 在文件中唯一匹配。"
                      "修改前请务必先用 read_file 或 search_code 确认原文。";
    def.parameters.push_back(makeParam("path", STRING, "相对工作区的文件路径"));
    def.parameters.push_back(makeParam("old_str", STRING, "要被替换的原文；留空表示覆盖写整个文件"));
    def.parameters.push_back(makeParam("new_str", STRING, "替换后的新内容；old_str 为空时它就是整个文件的内容"));
    def.parameters.push_back(makeParam("replace_all", BOOLEAN, "old_str 有多处匹配时是否全部替换，默认 false"));
    def.required_parameters.push_back("path");
    def.required_parameters.push_back("new_str");
    setToolDefine(def);
}

static string makeContextSnippet(const vector<string> &lines, int lineIndex, int radius)
{
    int begin = max(0, lineIndex - radius);
    int end = min((int)lines.size(), lineIndex + radius + 1);
    string text;
    for (int i = begin; i < end; i++)
        text += to_string(i + 1) + ": " + lines[i] + "\n";
    return text;
}

void JZModifyCodeTool::call(const ToolCall &params)
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

    string abs, error;
    if (!resolvePath(rel, abs, error))
    {
        finish(params, false, error, DenyReason::PathOutside);
        return;
    }

    emitLog("call", rel);

    bool isNew = oldStr.empty();
    string oldContent;
    bool existed = JZFileInfo::exists(abs);
    if (existed)
    {
        if (!isTextFile(abs))
        {
            finish(params, false, "不能修改二进制文件: " + rel);
            return;
        }
        oldContent = fileToString(abs, error);
        if (!error.empty())
        {
            finish(params, false, error);
            return;
        }
    }

    string newContent;
    int replaced = 0;
    if (isNew)
    {
        newContent = newStr;
    }
    else
    {
        if (!existed)
        {
            finish(params, false, "文件不存在，无法做替换: " + rel + "（如需新建请把 old_str 留空）",
                   DenyReason::NotFound);
            return;
        }

        size_t pos = 0;
        int count = 0;
        while ((pos = oldContent.find(oldStr, pos)) != string::npos)
        {
            count++;
            pos += oldStr.size();
        }

        if (count == 0)
        {
            finish(params, false, "在 " + rel + " 中找不到 old_str，请先用 read_file 确认原文（注意空格、换行与缩进必须完全一致）。",
                   DenyReason::EditFailed);
            return;
        }
        if (count > 1 && !replaceAll)
        {
            finish(params, false, "old_str 在 " + rel + " 中匹配了 " + to_string(count) +
                                      " 处，请补充更多上下文使其唯一，或设置 replace_all=true。",
                   DenyReason::EditFailed);
            return;
        }

        newContent = oldContent;
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
    }

    // 保留原文件的换行风格
    if (!newContent.empty() && oldContent.find("\r\n") != string::npos &&
        newContent.find("\r\n") == string::npos)
    {
        str_replace(newContent, "\n", "\r\n");
    }

    JZFile file;
    if (!file.open(abs, "wb"))
    {
        finish(params, false, "写入文件失败（无法打开）: " + rel);
        return;
    }
    int written = file.write(str_toUtf8(newContent));
    file.close();
    if (written != (int)newContent.size())
    {
        finish(params, false, "写入文件失败（写入长度不符）: " + rel);
        return;
    }

    string text;
    if (isNew)
    {
        text = (existed ? "已覆盖文件 " : "已新建文件 ") + rel + "，共 " +
               to_string(readLines(newContent).size()) + " 行。";
    }
    else
    {
        text = "已修改 " + rel + "，替换 " + to_string(replaced) + " 处。改动位置上下文:\n";
        vector<string> lines = readLines(newContent);
        int hitLine = -1;
        string firstLine = str_mid(newStr, 0, (int)newStr.find('\n'));
        for (int i = 0; i < (int)lines.size(); i++)
        {
            if (!firstLine.empty() && str_indexOf(lines[i], firstLine) != -1)
            {
                hitLine = i;
                break;
            }
        }
        if (hitLine >= 0)
            text += makeContextSnippet(lines, hitLine, 4);
    }

    finish(params, true, text);
}

// ==================== JZProcessRunner ====================
JZProcessRunner::~JZProcessRunner()
{
    stopAllProcesses();
}

bool JZProcessRunner::runProcess(const ToolCall &call, const string &program,
                                 const vector<string> &args, const string &workDir,
                                 int timeoutMs, const string &logText, bool emitCallLog)
{
    if (emitCallLog)
        emitLog("call", logText);

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

    emitLog("result", text);

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

void JZProcessRunner::stopAllProcesses()
{
    vector<JZProcess *> procs;
    for (const auto &t : m_tasks)
        procs.push_back(t.proc);

    for (auto *proc : procs)
        proc->killProcess();
}

// ==================== JZCommandTool ====================
JZCommandTool::JZCommandTool()
{
}

void JZCommandTool::setup(const string &name, const string &desc, const string &defaultCommand, int timeoutMs)
{
    ToolDefine def;
    def.name = name;
    def.description = desc;
    def.parameters.push_back(makeParam("command", STRING,
                                       defaultCommand.empty()
                                           ? string("要执行的完整命令（必填），如 cmake --build build 或 build/Debug/demo.exe；"
                                                    "Windows 下由 cmd /c 执行，支持管道、重定向与 && 串联")
                                           : ("要执行的完整命令；留空则使用默认命令(" + defaultCommand + ")；"
                                              "Windows 下由 cmd /c 执行")));
    def.parameters.push_back(makeParam("work_dir", STRING, "工作目录，相对工作区根目录，默认工作区根目录"));
    def.parameters.push_back(makeParam("timeout", NUMBER, "超时时间(毫秒)，默认 " + to_string(timeoutMs / 1000) + " 秒"));
    setToolDefine(def);

    m_defaultCommand = defaultCommand;
    m_timeoutMs = timeoutMs;
}

void JZCommandTool::setDefaultCommand(const string &cmd)
{
    m_defaultCommand = cmd;
}

const string &JZCommandTool::defaultCommand() const
{
    return m_defaultCommand;
}

void JZCommandTool::call(const ToolCall &params)
{
    string command = str_trimmed(params.params.get("command", "").asString());
    if (command.empty())
        command = m_defaultCommand;
    command = str_trimmed(command);
    if (command.empty())
    {
        finish(params, false, toolName() + " 没有可执行的命令：请在 command 参数中给出完整命令，或在网页设置里配置默认命令。");
        return;
    }

    string relDir = params.params.get("work_dir", "").asString();
    string workDir, error;
    if (!resolvePath(relDir.empty() ? "." : relDir, workDir, error))
    {
        finish(params, false, error, DenyReason::PathOutside);
        return;
    }

    int timeout = params.params.get("timeout", m_timeoutMs).asInt();
    if (timeout < 1000)
        timeout = 1000;

    string program;
    vector<string> args;
    if (!buildProcessArgs(command, program, args))
    {
        finish(params, false, "命令解析结果为空: " + command);
        return;
    }

    runProcess(params, program, args, workDir, timeout, command);
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

// ==================== JZVirtualTerminal ====================
JZVirtualTerminal::JZVirtualTerminal()
{
    // 规格表：P0 只有内建命令，shellTemplate 留空；
    // 后续 mkdir/rm/cp/mv 在此登记，并填上转发 cmd /c 的模板（flag 固定）。
    CommandSpec spec;
    spec.name = "pwd";
    spec.usage = "pwd              显示当前目录";
    m_specs["pwd"] = spec;

    spec = CommandSpec();
    spec.name = "cd";
    spec.usage = "cd [目录]        切换当前目录，省略目录则回到工作区根";
    spec.args.push_back({ArgKind::Path, Access::Read});
    m_specs["cd"] = spec;

    spec = CommandSpec();
    spec.name = "ls";
    spec.usage = "ls [目录]        列出目录内容，目录名以 \"/\" 结尾";
    spec.args.push_back({ArgKind::Path, Access::Read});
    m_specs["ls"] = spec;

    spec = CommandSpec();
    spec.name = "cat";
    spec.usage = "cat <文件>       显示文本文件内容（带行号，单次最多 2000 行）";
    spec.args.push_back({ArgKind::Path, Access::Read});
    m_specs["cat"] = spec;

    // ---- 写命令：路径审查通过后转发 shell，flag 在这里固定死 ----
    // copy / move 不加 /Y、del 不加 /Q 时会停下来等确认，直接把整个终端卡住。
#ifdef _WIN32
    spec = CommandSpec();
    spec.name = "mkdir";
    spec.usage = "mkdir <目录>     创建目录（可一次建多级）";
    spec.args.push_back({ArgKind::Path, Access::Write});
    spec.shellTemplate = "md {0}";
    m_specs["mkdir"] = spec;

    spec = CommandSpec();
    spec.name = "rmdir";
    spec.usage = "rmdir <目录>     删除空目录（非空目录请用 rm -r）";
    spec.args.push_back({ArgKind::Path, Access::Write});
    spec.shellTemplate = "rd /Q {0}";
    m_specs["rmdir"] = spec;

    spec = CommandSpec();
    spec.name = "rm";
    spec.usage = "rm [-r] <路径>   删除文件；加 -r 删除整个目录";
    spec.args.push_back({ArgKind::Path, Access::Write});
    spec.allowRecursive = true;
    spec.shellTemplate = "del /F /Q {0}";
    spec.shellTemplateDir = "rd /S /Q {0}";
    m_specs["rm"] = spec;

    spec = CommandSpec();
    spec.name = "cp";
    spec.usage = "cp <源> <目标>   复制文件";
    spec.args.push_back({ArgKind::Path, Access::Read});
    spec.args.push_back({ArgKind::Path, Access::Write});
    spec.shellTemplate = "copy /Y {0} {1}";
    m_specs["cp"] = spec;

    spec = CommandSpec();
    spec.name = "mv";
    spec.usage = "mv <源> <目标>   移动或重命名（源会消失，故源也必须在工作区内）";
    spec.args.push_back({ArgKind::Path, Access::Write});
    spec.args.push_back({ArgKind::Path, Access::Write});
    spec.shellTemplate = "move /Y {0} {1}";
    m_specs["mv"] = spec;
#else
    spec = CommandSpec();
    spec.name = "mkdir";
    spec.usage = "mkdir <目录>     创建目录（可一次建多级）";
    spec.args.push_back({ArgKind::Path, Access::Write});
    spec.shellTemplate = "mkdir -p {0}";
    m_specs["mkdir"] = spec;

    spec = CommandSpec();
    spec.name = "rmdir";
    spec.usage = "rmdir <目录>     删除空目录（非空目录请用 rm -r）";
    spec.args.push_back({ArgKind::Path, Access::Write});
    spec.shellTemplate = "rmdir {0}";
    m_specs["rmdir"] = spec;

    spec = CommandSpec();
    spec.name = "rm";
    spec.usage = "rm [-r] <路径>   删除文件；加 -r 删除整个目录";
    spec.args.push_back({ArgKind::Path, Access::Write});
    spec.allowRecursive = true;
    spec.shellTemplate = "rm -f {0}";
    spec.shellTemplateDir = "rm -rf {0}";
    m_specs["rm"] = spec;

    spec = CommandSpec();
    spec.name = "cp";
    spec.usage = "cp <源> <目标>   复制文件";
    spec.args.push_back({ArgKind::Path, Access::Read});
    spec.args.push_back({ArgKind::Path, Access::Write});
    spec.shellTemplate = "cp -f {0} {1}";
    m_specs["cp"] = spec;

    spec = CommandSpec();
    spec.name = "mv";
    spec.usage = "mv <源> <目标>   移动或重命名（源会消失，故源也必须在工作区内）";
    spec.args.push_back({ArgKind::Path, Access::Write});
    spec.args.push_back({ArgKind::Path, Access::Write});
    spec.shellTemplate = "mv -f {0} {1}";
    m_specs["mv"] = spec;
#endif

    // ---- 外部程序白名单 ----
    // code_agent 只服务 TypeScript / JavaScript：仅放行 git / node / tsc，
    // 其它语言(C++/Go/Python...)与 dotnet 的构建工具一律不放行。
    // tsc 指向 TS 环境目录里的原生编译器(TSGO 单文件版)，由设置注入全路径。
    // exeNames 同时给出带 .exe 与不带的写法，Windows / Linux 都能命中。
    // 只把"工具"登记进来；副作用大的单独归类，由策略开关控制。
    addProgram("git", {"git.exe", "git"}, CmdCategory::Tool,
               {"-C", "--exec", "-c", "--upload-pack", "--upload-archive"});
    addProgram("node", {"node.exe", "node"}, CmdCategory::Tool,
               {"-e", "--eval", "-p", "--print"});
    addProgram("tsc", {"tsc.exe", "tsc"}, CmdCategory::Tool);

    addProgram("pip", {"pip.exe", "pip"}, CmdCategory::PackageManager);
    addProgram("npm", {"npm.exe", "npm"}, CmdCategory::PackageManager);
    addProgram("npx", {"npx.exe", "npx"}, CmdCategory::PackageManager);
    addProgram("conda", {"conda.exe", "conda"}, CmdCategory::PackageManager);

    addProgram("curl", {"curl.exe", "curl"}, CmdCategory::Network);
    addProgram("wget", {"wget.exe", "wget"}, CmdCategory::Network);

    addProgram("reg", {"reg.exe", "reg"}, CmdCategory::SystemAdmin);
    addProgram("sc", {"sc.exe", "sc"}, CmdCategory::SystemAdmin);
    addProgram("net", {"net.exe", "net"}, CmdCategory::SystemAdmin);
    addProgram("setx", {"setx.exe", "setx"}, CmdCategory::SystemAdmin);
    addProgram("schtasks", {"schtasks.exe", "schtasks"}, CmdCategory::SystemAdmin);

    ToolDefine def;
    def.name = "terminal";
    def.description =
        "虚拟终端：像在终端里敲命令一样操作文件与目录。" \
        "注意它不是通用 shell，不支持管道(|)、重定向(>)与命令串联(&&)。" \
        "路径只能指向工作区与授信目录，越界会被拒绝；写操作只允许发生在工作区内。" \
        "同一个 terminal 名字会保留 cd 之后的目录，换个名字即可开一个独立终端。";
    def.parameters.push_back(makeParam("command", STRING, "要执行的命令，如 \"cd build\"、\"ls src\"、\"pwd\""));
    def.parameters.push_back(makeParam("terminal", STRING, "终端名，默认 default；不同名字各自维护当前目录"));
    def.required_parameters.push_back("command");
    setToolDefine(def);
}

void JZVirtualTerminal::setZoneConfig(const ZoneConfig &cfg)
{
    m_zone = cfg;
    m_zone.workspace = zoneNormalizeRoot(m_zone.workspace);
    for (auto &d : m_zone.trustedDirs)
        d.path = zoneNormalizeRoot(d.path);
    // 目录基准变了，已有会话的 cwd 可能失效，直接清空
    m_sessions.clear();
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

vector<string> JZVirtualTerminal::availablePrograms() const
{
    vector<string> out;
    for (const auto &it : m_programs)
    {
        string abs;
        if (findProgram(it.first, it.second.exeNames, abs))
            out.push_back(it.first);
    }
    return out;
}

void JZVirtualTerminal::setTrustedPrograms(const vector<TrustedProgram> &progs)
{
    m_trustedPrograms.clear();
    for (const auto &p : progs)
    {
        if (!p.name.empty() && !p.path.empty())
            m_trustedPrograms[str_toLower(p.name)] = p.path;
    }
}

const map<string, string> &JZVirtualTerminal::trustedPrograms() const
{
    return m_trustedPrograms;
}

JZVirtualTerminal::Session &JZVirtualTerminal::session(const string &name)
{
    auto it = m_sessions.find(name);
    if (it != m_sessions.end())
        return it->second;

    Session s;
    s.cwd = zoneNormalizeRoot(m_zone.workspace);
    return m_sessions.insert(make_pair(name, s)).first->second;
}

string JZVirtualTerminal::relToWorkspace(const string &abs) const
{
    string root = m_zone.workspace;
    if (!root.empty() && str_indexOf(abs, root) == 0)
    {
        string rel = abs.substr(root.size());
        return rel.empty() ? string(".") : rel;
    }
    return abs;
}

void JZVirtualTerminal::call(const ToolCall &params)
{
    string command = str_trimmed(params.params.get("command", "").asString());
    if (command.empty())
    {
        finish(params, false, "参数 command 不能为空");
        return;
    }

    vector<string> argv;
    string error;
    if (!splitCommandStrict(command, argv, error))
    {
        finish(params, false, error, DenyReason::CommandDenied);
        return;
    }
    if (argv.empty())
    {
        finish(params, false, "命令解析结果为空: " + command);
        return;
    }
    if (m_zone.workspace.empty())
    {
        finish(params, false, "工作区未配置，无法使用虚拟终端");
        return;
    }

    string termName = params.params.get("terminal", "default").asString();
    if (termName.empty())
        termName = "default";

    emitLog("call", command);

    string name = str_toLower(argv[0]);
    // 常见别名统一到主命令，这样 help 里只列一套
    if (name == "dir")  name = "ls";
    if (name == "type") name = "cat";
    if (name == "del")  name = "rm";
    if (name == "copy") name = "cp";
    if (name == "move") name = "mv";

    if (name == "help" || name == "?")
    {
        runHelp(params, argv);
        return;
    }

    auto it = m_specs.find(name);
    if (it == m_specs.end())
    {
        // 不是内建/转发命令 -> 当作外部程序（白名单，或工作区内带路径的可执行程序）
        Session &s = session(termName);
        runExternalProgram(params, argv, s);
        return;
    }

    Session &s = session(termName);
    if (name == "pwd")
        runPwd(params, argv, s);
    else if (name == "cd")
        runCd(params, argv, s);
    else if (name == "ls")
        runLs(params, argv, s);
    else if (name == "cat")
        runCat(params, argv, s);
    else if (!it->second.shellTemplate.empty())
        runShellCommand(params, name, it->second, argv, s);
    else
        runExternalProgram(params, argv, s);
}

void JZVirtualTerminal::runHelp(const ToolCall &call, const vector<string> &argv)
{
    string text = "可用命令:\n";
    for (const auto &it : m_specs)
        text += "  " + it.second.usage + "\n";
    text += "  help             显示本帮助\n";
    text += "\n当前工作区: " + m_zone.workspace + "\n";
    if (m_zone.trustedDirs.empty())
        text += "授信目录: (未配置，只能访问工作区)\n";
    else
    {
        text += "授信目录:\n";
        for (const auto &d : m_zone.trustedDirs)
            text += "  " + d.path + (d.writable ? "（可读写）" : "（只读）") + "\n";
    }

    // 只列在授信目录里真实存在的程序，避免模型去反复尝试没装的东西
    text += "\n可执行的程序:\n";
    int found = 0;
    for (const auto &it : m_programs)
    {
        string abs;
        if (findProgram(it.first, it.second.exeNames, abs))
        {
            text += "  " + it.first + "  (" + abs + ")\n";
            found++;
        }
    }
    if (found == 0)
        text += "  (无：未配置授信目录，或授信目录里没有找到已登记的程序)\n";
    text += "  工作区内编译出来的程序可直接带路径执行，如 build/Debug/demo.exe\n";

    if (!m_policy.allowPackageManager)
        text += "\n注意: 包管理命令(pip/npm/conda 等)与下载命令(curl/wget)当前被禁用。\n";

    finish(call, true, text);
}

void JZVirtualTerminal::runPwd(const ToolCall &call, const vector<string> &argv, Session &s)
{
    finish(call, true, s.cwd + "\n(工作区内相对路径: " + relToWorkspace(s.cwd) + ")");
}

void JZVirtualTerminal::runCd(const ToolCall &call, const vector<string> &argv, Session &s)
{
    if (argv.size() > 2)
    {
        finish(call, false, "cd 只接受一个参数: cd [目录]");
        return;
    }
    // 无参数时回到工作区根（对应真终端里 cd 回 home 的习惯）
    string target = (argv.size() == 2) ? argv[1] : m_zone.workspace;

    string abs, error;
    Zone zone;
    if (!zoneResolvePath(m_zone, s.cwd, target, abs, zone, error))
    {
        finish(call, false, error, DenyReason::PathOutside);
        return;
    }

    JZFileInfo info(abs);
    if (!info.exists() || !info.isDir())
    {
        finish(call, false, "不是目录: " + target, DenyReason::NotFound);
        return;
    }

    s.cwd = abs;
    finish(call, true, "当前目录: " + abs + "（" + zoneText(zone) + "）");
}

void JZVirtualTerminal::runLs(const ToolCall &call, const vector<string> &argv, Session &s)
{
    if (argv.size() > 2)
    {
        finish(call, false, "ls 只接受一个参数: ls [目录]");
        return;
    }
    string target = (argv.size() == 2) ? argv[1] : ".";

    string abs, error;
    Zone zone;
    if (!zoneResolvePath(m_zone, s.cwd, target, abs, zone, error))
    {
        finish(call, false, error, DenyReason::PathOutside);
        return;
    }

    JZFileInfo info(abs);
    if (!info.exists())
    {
        finish(call, false, "路径不存在: " + target, DenyReason::NotFound);
        return;
    }
    if (!info.isDir())
    {
        finish(call, true, target + "  是文件 (" + to_string(info.size()) + " 字节)");
        return;
    }

    JZDir dir(abs);
    vector<JZFileInfo> list = dir.entryInfoList(JZDir::Files | JZDir::Dirs | JZDir::NoDotAndDotDot,
                                                JZDir::SortName);
    string text = "目录 " + abs + "（" + zoneText(zone) + "）:\n";
    int dirCount = 0, fileCount = 0;
    for (const auto &e : list)
    {
        if (e.isDir())
        {
            text += "  " + e.fileName() + "/\n";
            dirCount++;
        }
        else
        {
            text += "  " + e.fileName() + "\n";
            fileCount++;
        }
    }
    text += "共 " + to_string(dirCount) + " 个目录, " + to_string(fileCount) + " 个文件";
    finish(call, true, text);
}

void JZVirtualTerminal::runCat(const ToolCall &call, const vector<string> &argv, Session &s)
{
    if (argv.size() != 2)
    {
        finish(call, false, "用法: cat <文件>");
        return;
    }
    string target = argv[1];

    string abs, error;
    Zone zone;
    if (!zoneResolvePath(m_zone, s.cwd, target, abs, zone, error))
    {
        finish(call, false, error, DenyReason::PathOutside);
        return;
    }

    JZFileInfo info(abs);
    if (!info.exists() || !info.isFile())
    {
        finish(call, false, "不是文件: " + target, DenyReason::NotFound);
        return;
    }
    if (info.size() > 2 * 1024 * 1024)
    {
        finish(call, false, "文件过大(>2MB): " + target);
        return;
    }
    if (!isTextFile(abs))
    {
        finish(call, false, "不是文本文件: " + target);
        return;
    }

    string err;
    string content = fileToString(abs, err);
    if (!err.empty())
    {
        finish(call, false, err);
        return;
    }

    vector<string> lines = readLines(content);
    const int kMaxLines = 2000;
    bool truncated = (int)lines.size() > kMaxLines;
    int end = truncated ? kMaxLines : (int)lines.size();

    string text = "文件 " + abs + "（共 " + to_string(lines.size()) + " 行）:\n";
    for (int i = 0; i < end; i++)
        text += to_string(i + 1) + ": " + lines[i] + "\n";
    if (truncated)
        text += "... 单次最多显示 " + to_string(kMaxLines) + " 行\n";

    finish(call, true, text);
}

bool JZVirtualTerminal::checkShellSafe(const string &path, string &error)
{
    // 拼回命令行时路径会被引号包住，路径自身含引号会破坏配对
    if (str_contains(path, "\""))
    {
        error = "路径不允许包含双引号: " + path;
        return false;
    }
    return true;
}

bool JZVirtualTerminal::resolveArg(const vector<string> &argv, int index, const Session &s,
                                   bool writeAccess, string &abs, string &error, DenyReason &deny)
{
    string raw = argv[index];
    deny = DenyReason::None;

    // 通配符交给 shell 展开，但必须保证展开范围落在工作区内
    if (str_contains(raw, "*") || str_contains(raw, "?"))
    {
        size_t pos = raw.find_last_of("/\\");
        string parent = (pos == string::npos) ? string(".") : raw.substr(0, pos);
        if (parent.empty())
            parent = ".";

        string parentAbs, e;
        Zone z;
        if (!zoneResolvePath(m_zone, s.cwd, parent, parentAbs, z, e))
        {
            error = "通配符的展开范围越界: " + raw + "（" + e + "）";
            deny = DenyReason::PathOutside;
            return false;
        }
        if (z != Zone::Workspace)
        {
            error = "通配符只能在工区内使用（展开范围必须是工作区）: " + raw;
            deny = DenyReason::PathOutside;
            return false;
        }
        abs = raw;    // 保留原样，由 shell 在 cwd 下展开
        return true;
    }

    string a, e;
    Zone z;
    if (!zoneResolvePath(m_zone, s.cwd, raw, a, z, e))
    {
        error = e;
        deny = DenyReason::PathOutside;
        return false;
    }
    if (writeAccess)
    {
        string e2;
        if (!zoneCheckWrite(m_zone, a, e2))
        {
            error = e2;
            deny = DenyReason::PathOutside;
            return false;
        }
    }
    abs = a;
    return true;
}

// 递归统计目录下的文件数，超过 limit 提前返回（避免为了计数遍历整个大目录）
static int countDirFiles(const string &absDir, int limit)
{
    JZDir dir(absDir);
    if (!dir.exists())
        return 0;

    int count = 0;
    vector<JZFileInfo> list = dir.entryInfoList(JZDir::Files | JZDir::Dirs | JZDir::NoDotAndDotDot);
    for (const auto &info : list)
    {
        if (info.isDir())
            count += countDirFiles(info.filePath(), limit);
        else
            count++;

        if (count > limit)
            return count;
    }
    return count;
}

void JZVirtualTerminal::runShellCommand(const ToolCall &call, const string &name,
                                        const CommandSpec &spec, const vector<string> &argv,
                                        Session &s)
{
    // 1. 分出选项与路径参数
    vector<string> operands;
    bool recursive = false;
    for (size_t i = 1; i < argv.size(); i++)
    {
        string a = argv[i];
        if (a.size() > 1 && a[0] == '-')
        {
            string f = str_toLower(a);
            if ((f == "-r" || f == "-rf" || f == "-fr" || f == "--recursive") && spec.allowRecursive)
                recursive = true;
            else
            {
                finish(call, false, "不支持的选项: " + a + "\n用法: " + spec.usage);
                return;
            }
        }
        else
        {
            operands.push_back(a);
        }
    }

    if (operands.size() != spec.args.size())
    {
        finish(call, false, "用法: " + spec.usage);
        return;
    }

    // 2. 逐个解析路径并校验读写权限
    vector<string> absList;
    for (size_t i = 0; i < operands.size(); i++)
    {
        string abs, error;
        DenyReason deny = DenyReason::None;
        if (!resolveArg(operands, (int)i, s, spec.args[i].access == Access::Write, abs, error, deny))
        {
            finish(call, false, error, deny);
            return;
        }
        absList.push_back(abs);
    }

    // 3. 目标是已存在目录时改用目录模板（rm -r 走 rd /S /Q）
    string tmpl = spec.shellTemplate;
    if (!spec.shellTemplateDir.empty())
    {
        JZFileInfo info(absList[0]);
        if (info.exists() && info.isDir())
        {
            if (!recursive)
            {
                finish(call, false, absList[0] + " 是目录，删除目录请加 -r");
                return;
            }
            tmpl = spec.shellTemplateDir;
        }
    }

    // 4. 删除类的兜底保护：不许删工作区根，且 -r 有数量上限
    if (name == "rm" || name == "rmdir")
    {
        for (const auto &p : absList)
        {
            if (str_toLower(p) == str_toLower(m_zone.workspace))
            {
                finish(call, false, "不允许删除工作区根目录: " + p);
                return;
            }
        }
        if (recursive && !str_contains(absList[0], "*") && !str_contains(absList[0], "?"))
        {
            const int kMaxDeleteFiles = 200;
            int count = countDirFiles(absList[0], kMaxDeleteFiles);
            if (count > kMaxDeleteFiles)
            {
                finish(call, false, "待删除文件数超过上限 " + to_string(kMaxDeleteFiles) +
                                        "（当前 " + to_string(count) + " 个），已阻止。" \
                                        "确认要删请缩小范围后分批执行。");
                return;
            }
        }
    }

    // 5. 拼命令行：路径一律加引号（含空格的路径不加引号会被切断）
    string cmdline = tmpl;
    for (size_t i = 0; i < absList.size(); i++)
    {
        string error;
        if (!checkShellSafe(absList[i], error))
        {
            finish(call, false, error);
            return;
        }
        str_replace(cmdline, "{" + to_string(i) + "}", "\"" + absList[i] + "\"");
    }

    // 6. 转发给系统 shell（flag 已在模板里固定，模型无法注入）
    string program;
    vector<string> args;
    buildProcessArgs(cmdline, program, args);

    const int kShellTimeoutMs = 60000;
    runProcess(call, program, args, s.cwd, kShellTimeoutMs, name + ": " + cmdline, false);
}

void JZVirtualTerminal::addProgram(const string &name, vector<string> exeNames,
                                   CmdCategory category,
                                   vector<string> denyFlags, vector<string> denyModules)
{
    ProgramSpec ps;
    ps.name = name;
    ps.exeNames = exeNames;
    ps.category = category;
    ps.denyFlags = denyFlags;
    ps.denyModules = denyModules;
    m_programs[name] = ps;
}

bool JZVirtualTerminal::findProgram(const string &name, const vector<string> &exeNames, string &abs) const
{
    // 1. 显式配置的可信程序（全路径）优先：用户已确认信任该路径
    auto pit = m_trustedPrograms.find(str_toLower(name));
    if (pit != m_trustedPrograms.end() && !pit->second.empty())
    {
        if (JZFileInfo::exists(pit->second))
        {
            abs = codeNormalizePath(pit->second);
            return true;
        }
        return false;   // 配置了但文件不存在，不要退化到目录搜索（避免指向错误程序）
    }

    // 2. 在授信目录里按 exe 名搜索
    for (const auto &d : m_zone.trustedDirs)
    {
        if (d.path.empty())
            continue;
        for (const auto &exe : exeNames)
        {
            string p = d.path + "/" + exe;
            if (JZFileInfo::exists(p))
            {
                abs = codeNormalizePath(p);
                return true;
            }
        }
    }
    return false;
}

bool JZVirtualTerminal::checkCategory(CmdCategory cat, string &error) const
{
    switch (cat)
    {
    case CmdCategory::PackageManager:
        if (!m_policy.allowPackageManager)
        {
            error = "已阻止：包管理命令会安装或升级依赖，当前不允许执行（防止改动环境）。" \
                    "如确需执行，请让用户在设置中开启。";
            return false;
        }
        return true;
    case CmdCategory::Network:
        if (!m_policy.allowNetwork)
        {
            error = "已阻止：网络下载命令当前不允许执行。" \
                    "如确需执行，请让用户在设置中开启。";
            return false;
        }
        return true;
    case CmdCategory::SystemAdmin:
        error = "已阻止：系统管理命令（修改注册表/服务/环境变量/计划任务）不允许执行。";
        return false;
    default:
        return true;
    }
}

// 检查受限参数：前缀匹配（如 git -c / --exec），外加 -m <module> 的组合判断
static bool hasDenyFlag(const vector<string> &argv, const ProgramSpec &spec, string &bad)
{
    for (size_t i = 0; i < argv.size(); i++)
    {
        string a = str_toLower(argv[i]);
        for (const auto &f : spec.denyFlags)
        {
            if (a == f || (f.size() >= 2 && str_indexOf(a, f) == 0))
            {
                bad = argv[i];
                return true;
            }
        }
        // python -m pip 这类绕法单独挡：模块名逐个比
        if (a == "-m" && i + 1 < argv.size())
        {
            string mod = str_toLower(argv[i + 1]);
            for (const auto &m : spec.denyModules)
            {
                if (mod == m)
                {
                    bad = "-m " + argv[i + 1];
                    return true;
                }
            }
        }
    }
    return false;
}

void JZVirtualTerminal::runExternalProgram(const ToolCall &call, const vector<string> &argv, Session &s)
{
    string first = argv[0];
    string abs;

    if (str_contains(first, "/") || str_contains(first, "\\") || codeIsAbsPath(first))
    {
        // 显式给出路径：必须是工作区/授信区内的可执行程序（工作区里的编译产物走这条路）
        string error;
        Zone zone;
        if (!zoneResolvePath(m_zone, s.cwd, first, abs, zone, error))
        {
            finish(call, false, "不允许执行该路径下的程序: " + first + "（" + error + "）",
                   DenyReason::ProgramDenied);
            return;
        }
        JZFileInfo info(abs);
        if (!info.isFile())
        {
            finish(call, false, "程序不存在: " + first, DenyReason::NotFound);
            return;
        }
        string suffix = str_toLower(info.suffix());
        if (suffix != "exe" && suffix != "bat" && suffix != "cmd")
        {
            finish(call, false, "不是可执行程序(.exe/.bat/.cmd): " + first);
            return;
        }
    }
    else
    {
        string progName = str_toLower(first);
        if (str_endsWith(progName, ".exe"))
            progName = progName.substr(0, progName.size() - 4);

        auto it = m_programs.find(progName);
        if (it == m_programs.end())
        {
            finish(call, false,
                   "不允许执行程序: " + first + "。可执行程序请用 \"help\" 查看；" \
                   "若是工作区里编译出来的程序，请带上路径，如 build/Debug/demo.exe。",
                   DenyReason::ProgramDenied);
            return;
        }

        const ProgramSpec &ps = it->second;
        string policyError;
        if (!checkCategory(ps.category, policyError))
        {
            finish(call, false, policyError, DenyReason::EnvChange);
            return;
        }

        vector<string> rest(argv.begin() + 1, argv.end());
        string bad;
        if (hasDenyFlag(rest, ps, bad))
        {
            finish(call, false, "该参数被禁止: " + bad + "（" + ps.name + " 的受限参数）",
                   DenyReason::CommandDenied);
            return;
        }

        if (!findProgram(ps.name, ps.exeNames, abs))
        {
            string dirs;
            for (const auto &d : m_zone.trustedDirs)
                dirs += (dirs.empty() ? string() : string(", ")) + d.path;
            if (dirs.empty())
                dirs = "(未配置授信目录)";

            // 单独发一条 notice：模型不一定转述，用户要能在界面上直接看到缺什么
            emitLog("notice", "缺少程序 " + ps.name + "：请在安装后把它的安装目录加为授信目录。");
            finish(call, false,
                   "未找到程序 " + ps.name + "。已在授信目录中查找: " + dirs + "。" \
                   "不要换其它写法重试；请直接告诉用户需要安装该程序，并说明装好后" \
                   "要在设置里把安装目录加为授信目录。",
                   DenyReason::NotFound);
            return;
        }
    }

    vector<string> args(argv.begin() + 1, argv.end());
    string program = abs;

    // CreateProcess 不能直接跑 .bat/.cmd，必须经 shell
    string suffix = str_toLower(JZFileInfo(abs).suffix());
    if (suffix == "bat" || suffix == "cmd")
    {
        string cmdline = "\"" + abs + "\"";
        for (const auto &a : args)
            cmdline += " \"" + a + "\"";
        buildProcessArgs(cmdline, program, args);
    }

    string logText;
    for (const auto &a : argv)
        logText += (logText.empty() ? string() : string(" ")) + a;

    const int kProgramTimeoutMs = 300000;   // 5 分钟
    runProcess(call, program, args, s.cwd, kProgramTimeoutMs, logText, false);
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

JZWebSearchTool::JZWebSearchTool()
{
    ToolDefine def;
    def.name = "web_search";
    def.description = "联网搜索：给定查询词，返回若干条网页搜索结果（标题、链接、摘要）。" \
                      "适合查找官方文档、API 用法、报错原因、第三方库等需要实时信息的问题。";
    def.parameters.push_back(makeParam("query", STRING, "搜索关键词"));
    def.parameters.push_back(makeParam("num_results", NUMBER, "返回几条结果，默认 5，最多 20"));
    def.required_parameters.push_back("query");
    setToolDefine(def);
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

    string url = m_searchUrlTemplate;
    if (url.empty())
        url = "https://api.duckduckgo.com/?q={q}&format=json&no_html=1&skip_disambig=1";

    string enc = urlEncodeQuery(query);
    for (;;)
    {
        size_t pos = url.find("{q}");
        if (pos == string::npos)
            break;
        url.replace(pos, 3, enc);
    }

    emitLog("call", query);

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
    m_reply = m_net->get(req);
    if (!m_reply)
    {
        finish(params, false, "创建网络请求失败");
        return;
    }

    JZNetworkReply *reply = m_reply;
    connect(&reply->sigFinished, this, [this, reply, params, num]()
    {
        if (m_reply == reply)
            m_reply = nullptr;
        handleReply(reply, params, num);
    });
}

void JZWebSearchTool::handleReply(JZNetworkReply *reply, const ToolCall &call, int num)
{
    JZNetworkReply::NetworkError err = reply->error();
    if (err != JZNetworkReply::NoError && err != JZNetworkReply::None)
    {
        finish(call, false, "网络搜索失败: " + reply->errorString());
        return;
    }
    int code = reply->httpStatusCode();
    if (code < 200 || code >= 300)
    {
        finish(call, false, "搜索服务返回 HTTP " + to_string(code) +
               (code == 0 ? "（无法连接，请检查网络/代理设置）" : string()));
        return;
    }

    string body = str_fromUtf8(reply->readAll());
    parseSearch(body, call, num);
}

void JZWebSearchTool::parseSearch(const string &body, const ToolCall &call, int num)
{
    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(body, root))
    {
        // 非 JSON：多半是登录页/错误页，原样回吐前若干字符便于排查
        string snippet = body.size() > 2000 ? body.substr(0, 2000) : body;
        finish(call, true, "搜索服务返回内容非 JSON（可能是登录页或错误页），原文前 2000 字符:\n" + snippet);
        return;
    }

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

    vector<string> results;
    if (arr)
    {
        for (int i = 0; i < (int)arr->size(); i++)
        {
            const Json::Value &it = (*arr)[i];
            if (!it.isObject())
                continue;

            // DuckDuckGo 的 RelatedTopics 里还嵌套了 Topics，跳过只有子列表的项
            if (it.isMember("Topics") && it["Topics"].isArray() && !it.isMember("FirstURL") && !it.isMember("Text"))
                continue;

            string title = searchField(it, {"title", "Title", "heading", "result"});
            string url   = searchField(it, {"url", "URL", "href", "link", "FirstURL"});
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
                results.push_back(line);
        }
    }

    if (results.empty())
    {
        finish(call, true, "未从搜索服务获取到结果（可能无匹配，或服务返回了空列表）。");
        return;
    }

    int n = min(num, (int)results.size());
    string text = "网络搜索结果（共 " + to_string(results.size()) + " 条，以下展示前 " +
                  to_string(n) + " 条）:\n";
    for (int i = 0; i < n; i++)
        text += to_string(i + 1) + ". " + results[i] + "\n";
    finish(call, true, text);
}

// ==================== JZWebFetchTool ====================
// 把一段响应字节按声明的字符集转成 UTF-8：
//   - utf-8 / 未声明：直接按 UTF-8 解码
//   - 其它（gbk / gb2312 / big5 等）：经 str_local_to_utf8 按系统本地代码页转 UTF-8
//     （中文系统本地代码页为 936，可正确解 gbk；非中文系统为尽力而为）
// 注：本文件已包含 JZAgentTool.h，其枚举含 BOOLEAN，不能再 include <windows.h>，
//     故复用 JZStd 里的 str_local_to_utf8（其 windows 调用在独立 TU，不冲突）。
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
    def.parameters.push_back(makeParam("url", STRING, "要抓取的网页 URL（http/https）"));
    def.parameters.push_back(makeParam("max_chars", NUMBER, "返回正文的最大字符数，默认 20000，最多 200000"));
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

    emitLog("call", url);

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

