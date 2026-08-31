#include "JZCodeTools.h"

#include <algorithm>

#include "JZDir.h"
#include "JZFile.h"
#include "JZFileInfo.h"
#include "JZPlatform.h"
#include "JZRegExp.h"
#include "JZStd.h"
#include "JZString.h"
#include "JZTextStream.h"

using namespace std;

// ==================== 基础工具函数 ====================

// 统一分隔符为 '/'，并消除 "." / ".."。
// escaped 不为空时，记录是否有 ".." 试图越过起始目录（调用方据此判越界，而不是静默夹住）
static string codeNormalizePath(const string &path, bool *escaped = nullptr)
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

// 解析命令行: 支持双引号/单引号包裹含空格的参数
static vector<string> splitCommandLine(const string &cmd)
{
    vector<string> out;
    string cur;
    bool inQuote = false;
    char quoteChar = 0;

    for (size_t i = 0; i < cmd.size(); i++)
    {
        char c = cmd[i];
        if (inQuote)
        {
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
            else if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            {
                if (!cur.empty())
                {
                    out.push_back(cur);
                    cur.clear();
                }
            }
            else
            {
                cur.push_back(c);
            }
        }
    }
    if (!cur.empty())
        out.push_back(cur);
    return out;
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

void JZCodeTool::emitLog(const string &type, const string &text)
{
    sigToolLog.emit(type, m_toolDefine.name, text);
}

void JZCodeTool::finish(const ToolCall &call, bool ok, const string &text)
{
    emitLog("result", text);

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
        finish(params, false, error);
        return;
    }

    JZFileInfo info(abs);
    if (!info.exists())
    {
        finish(params, false, "路径不存在: " + rel);
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
        finish(params, false, error);
        return;
    }

    emitLog("call", rel);

    JZFileInfo info(abs);
    if (!info.exists() || !info.isFile())
    {
        finish(params, false, "文件不存在: " + rel);
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
        finish(params, false, error);
        return;
    }

    JZFileInfo info(abs);
    if (!info.exists())
    {
        finish(params, false, "路径不存在: " + rel);
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
        finish(params, false, error);
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
            finish(params, false, "文件不存在，无法做替换: " + rel + "（如需新建请把 old_str 留空）");
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
            finish(params, false, "在 " + rel + " 中找不到 old_str，请先用 read_file 确认原文（注意空格、换行与缩进必须完全一致）。");
            return;
        }
        if (count > 1 && !replaceAll)
        {
            finish(params, false, "old_str 在 " + rel + " 中匹配了 " + to_string(count) +
                                      " 处，请补充更多上下文使其唯一，或设置 replace_all=true。");
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

// ==================== JZCommandTool ====================
JZCommandTool::JZCommandTool()
{
}

JZCommandTool::~JZCommandTool()
{
    stopAll();
}

void JZCommandTool::setup(const string &name, const string &desc, const string &defaultCommand, int timeoutMs)
{
    ToolDefine def;
    def.name = name;
    def.description = desc;
    def.parameters.push_back(makeParam("command", STRING,
                                       "要执行的完整命令；留空则使用设置中的默认命令(" +
                                           (defaultCommand.empty() ? "未配置" : defaultCommand) + ")"));
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
        finish(params, false, error);
        return;
    }

    int timeout = params.params.get("timeout", m_timeoutMs).asInt();
    if (timeout < 1000)
        timeout = 1000;

    vector<string> argv = splitCommandLine(command);
    if (argv.empty())
    {
        finish(params, false, "命令解析结果为空: " + command);
        return;
    }

    string program = argv[0];
    vector<string> args(argv.begin() + 1, argv.end());
#ifdef _WIN32
    // CreateProcess 的命令行里, 含空格的可执行文件路径必须带引号
    if (program.find(' ') != string::npos && program.front() != '"')
        program = "\"" + program + "\"";
#endif

    emitLog("call", command);

    JZProcess *proc = new JZProcess(this);
    proc->setWorkingDirectory(workDir);

    string tool_name = toolName();
    string call_id = params.id;
    connect(&proc->sigFinished, this,
            [this, tool_name, call_id, proc](int code, JZProcess::ExitStatus status)
            {
                onProcFinished(proc, tool_name, call_id, code, status);
            });

    if (!proc->start(program, args))
    {
        proc->deleteLater();
        finish(params, false, "启动进程失败: " + command + "\n请确认命令中的程序存在且可运行。");
        return;
    }

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
    timer->start(timeout);

    Task task;
    task.id = params.id;
    task.proc = proc;
    task.timer = timer;
    task.startTime = JZPlatform::getTickCountMs();
    m_tasks.push_back(task);
}

void JZCommandTool::onProcFinished(JZProcess *proc, const string &name, const string &id,
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

void JZCommandTool::removeTask(JZProcess *proc)
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

void JZCommandTool::stopAll()
{
    vector<JZProcess *> procs;
    for (const auto &t : m_tasks)
        procs.push_back(t.proc);

    for (auto *proc : procs)
        proc->killProcess();
}
