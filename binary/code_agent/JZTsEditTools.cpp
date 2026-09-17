#include "JZTsEditTools.h"

#include <algorithm>

#include "JZCodeAgent.h"
#include "JZDir.h"
#include "JZFile.h"
#include "JZFileInfo.h"
#include "JZStd.h"
#include "json/json.h"

using namespace std;

// ============================================================
// 本地文本辅助（行级编辑；行内容不含行尾符，行尾风格整体保留）
// ============================================================
static vector<string> splitTextLines(const string &text, bool &crlf)
{
    crlf = text.find("\r\n") != string::npos;
    vector<string> lines;
    string cur;
    for (char c : text)
    {
        if (c == '\n')
        {
            if (!cur.empty() && cur.back() == '\r')
                cur.pop_back();
            lines.push_back(cur);
            cur.clear();
        }
        else
        {
            cur += c;
        }
    }
    if (!cur.empty())
        lines.push_back(cur);
    return lines;
}

static string joinTextLines(const vector<string> &lines, bool crlf)
{
    string eol = crlf ? "\r\n" : "\n";
    string out;
    for (size_t i = 0; i < lines.size(); i++)
    {
        if (i > 0)
            out += eol;
        out += lines[i];
    }
    return out;
}

static bool readFileText(const string &abs, string &text)
{
    JZFile f;
    if (!f.open(abs, "rb"))
        return false;
    JZByteArray data = f.readAll();
    f.close();
    text = str_fromUtf8(data);
    return true;
}

static bool writeFileText(const string &abs, const string &text)
{
    JZFile f;
    if (!f.open(abs, "wb"))
        return false;
    f.write(str_toUtf8(text));
    f.close();
    return true;
}

static bool isBlankLine(const string &s)
{
    return str_trimmed(s).empty();
}

static string leadingWs(const string &s)
{
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
        i++;
    return s.substr(0, i);
}

// 把 code 的每行剥掉公共前导空白（保留相对缩进），再统一加上目标缩进前缀。
// 这样模型给到的片段无论带什么缩进，落点缩进都是对的。
static vector<string> reindentCode(const string &code, const string &indent)
{
    vector<string> raw;
    bool dummy = false;
    {
        // 复用 splitTextLines（忽略行尾风格：新片段按 \n 拆）
        bool c = false;
        raw = splitTextLines(code, c);
        (void)dummy;
    }
    // 公共前导空白（忽略空行）
    string common;
    bool first = true;
    for (const auto &l : raw)
    {
        if (isBlankLine(l))
            continue;
        string ws = leadingWs(l);
        if (first)
        {
            common = ws;
            first = false;
        }
        else
        {
            size_t n = min(common.size(), ws.size());
            size_t k = 0;
            while (k < n && common[k] == ws[k])
                k++;
            common = common.substr(0, k);
        }
    }
    vector<string> out;
    for (const auto &l : raw)
    {
        if (isBlankLine(l))
            out.push_back(string());
        else
        {
            string body = l.substr(min(common.size(), l.size()));
            out.push_back(indent + body);
        }
    }
    // 去掉首尾空行
    while (!out.empty() && out.front().empty())
        out.erase(out.begin());
    while (!out.empty() && out.back().empty())
        out.pop_back();
    return out;
}

static bool validIdent(const string &s)
{
    if (s.empty())
        return false;
    for (size_t i = 0; i < s.size(); i++)
    {
        char c = s[i];
        bool ok = (c == '_' || c == '$' || isalpha((unsigned char)c)) ||
                  (i > 0 && isdigit((unsigned char)c));
        if (!ok)
            return false;
    }
    return true;
}

// LSP 列偏移是 UTF-16 码元数：把一行内 UTF-16 偏移换算成 UTF-8 字节偏移
static size_t utf16ToByte(const string &utf8, size_t units)
{
    size_t i = 0, u = 0;
    while (i < utf8.size() && u < units)
    {
        unsigned char c = (unsigned char)utf8[i];
        size_t len = (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
        if (i + len > utf8.size())
            break;
        unsigned int cp = 0;
        if (len == 1)
            cp = c;
        else if (len == 2)
            cp = ((c & 0x1F) << 6) | (utf8[i + 1] & 0x3F);
        else if (len == 3)
            cp = ((c & 0x0F) << 12) | ((utf8[i + 1] & 0x3F) << 6) | (utf8[i + 2] & 0x3F);
        else
            cp = ((c & 0x07) << 18) | ((utf8[i + 1] & 0x3F) << 12) |
                 ((utf8[i + 2] & 0x3F) << 6) | (utf8[i + 3] & 0x3F);
        u += (cp >= 0x10000) ? 2 : 1;
        i += len;
    }
    return i;
}

// ============================================================
// 符号树定位（与查询组的 findSymbolByName 同语义：先序首个匹配，
// class/interface 优先；绝不让深层同名覆盖浅层符号）
// ============================================================
static Json::Value findNode(const Json::Value &arr, const string &name, bool preferType)
{
    for (const auto &s : arr)
    {
        if (!s.isObject())
            continue;
        string kind = s.get("kind", "").asString();
        bool isType = (kind == "class" || kind == "interface");
        if (s.get("name", "").asString() == name && (!preferType || isType))
            return s;
        Json::Value deeper = findNode(
            s.get("children", Json::Value(Json::arrayValue)), name, preferType);
        if (!deeper.isNull())
            return deeper;
    }
    return Json::Value();
}

// cls 为空 -> 顶层找 fn；fn 为空 -> 找类 cls；两者都有 -> 类 cls 的成员 fn
static Json::Value locateTarget(const Json::Value &tree, const string &cls,
                                const string &fn, string &err)
{
    if (fn.empty())
    {
        Json::Value n = findNode(tree, cls, true);
        if (n.isNull())
            n = findNode(tree, cls, false);
        if (n.isNull())
            err = "文件中未找到类/接口 \"" + cls + "\"，可先用 ts_outline(file) 查看符号";
        return n;
    }
    if (!cls.empty())
    {
        Json::Value c = findNode(tree, cls, true);
        if (c.isNull())
            c = findNode(tree, cls, false);
        if (c.isNull())
        {
            err = "文件中未找到类/接口 \"" + cls + "\"，可先用 ts_outline(file) 查看符号";
            return Json::Value();
        }
        Json::Value m = findNode(c.get("children", Json::Value(Json::arrayValue)), fn, false);
        if (m.isNull())
        {
            err = "类 " + cls + " 中未找到成员 \"" + fn + "\"，可用 ts_members(file, \"" +
                  cls + "\") 查看成员";
            return Json::Value();
        }
        return m;
    }
    Json::Value n = findNode(tree, fn, false);
    if (n.isNull())
        err = "文件中未找到顶层函数 \"" + fn + "\"，可先用 ts_outline(file) 查看符号";
    return n;
}

// ============================================================
// LSP rename 编辑应用（UTF-16 列偏移 -> 字节；按 end 位置倒序应用）
// ============================================================
struct TextEdit1
{
    int line = 0, offset = 0, endLine = 0, endOffset = 0; // 1-based
    string text;
};

static bool applyRenameEdits(vector<string> &lines, vector<TextEdit1> edits, string &err)
{
    sort(edits.begin(), edits.end(), [](const TextEdit1 &a, const TextEdit1 &b) {
        if (a.endLine != b.endLine)
            return a.endLine > b.endLine;
        return a.endOffset > b.endOffset;
    });

    int lastEndLine = INT_MAX;
    size_t lastEndByte = 0;
    for (const auto &e : edits)
    {
        int li = e.line - 1, eli = e.endLine - 1;
        if (li < 0 || eli >= (int)lines.size() || li > eli)
        {
            err = "编辑位置越界 (" + to_string(e.line) + ")";
            return false;
        }
        // 同一行内多个编辑必须从右往左应用（倒序已保证）；跨行编辑不允许区间重叠
        if (e.endLine == lastEndLine && e.endOffset > (int)lastEndByte)
        {
            err = "编辑区间重叠";
            return false;
        }
        lastEndLine = e.endLine;
        lastEndByte = utf16ToByte(lines[eli], (size_t)max(0, e.endOffset - 1));

        if (li == eli)
        {
            string &L = lines[li];
            size_t b0 = utf16ToByte(L, (size_t)max(0, e.offset - 1));
            size_t b1 = utf16ToByte(L, (size_t)max(0, e.endOffset - 1));
            if (b0 > L.size() || b1 > L.size() || b0 > b1)
            {
                err = "编辑列偏移越界 (行 " + to_string(e.line) + ")";
                return false;
            }
            L = L.substr(0, b0) + e.text + L.substr(b1);
        }
        else
        {
            string first = lines[li], last = lines[eli];
            size_t b0 = utf16ToByte(first, (size_t)max(0, e.offset - 1));
            size_t b1 = utf16ToByte(last, (size_t)max(0, e.endOffset - 1));
            if (b0 > first.size() || b1 > last.size())
            {
                err = "编辑列偏移越界 (" + to_string(e.line) + "~" + to_string(e.endLine) + ")";
                return false;
            }
            lines[li] = first.substr(0, b0) + e.text + last.substr(b1);
            lines.erase(lines.begin() + li + 1, lines.begin() + eli + 1);
        }
    }
    return true;
}

// ============================================================
// 工具定义
// ============================================================
JZTsEditTool::JZTsEditTool(JZTsLangServer *server, Kind kind, ICodeToolHost *host)
    : m_server(server), m_kind(kind)
{
    setHost(host); // 空指针也安全：checkWrite / checkPathPermission 判空跳过
    ToolDefine def;
    ParamSchema file = makeParam("file", ParamType::String, "TypeScript/JavaScript 文件路径（相对工作区或绝对）");
    ParamSchema scope = makeParam("scope", ParamType::String,
                                  "编辑范围：\"文件路径\"（顶层符号）或 \"文件路径#类名\"（类成员）");
    ParamSchema name = makeParam("name", ParamType::String, "符号名（类名 / 函数名 / 方法名）");
    ParamSchema newName = makeParam("new_name", ParamType::String, "新名字（合法标识符）");
    ParamSchema code = makeParam("code", ParamType::String,
                                 "完整的声明文本（可带任意缩进，工具会按落点自动重排）");

    switch (kind)
    {
        case Kind::Summary:
            def.name = "ts_summary";
            def.description = "列出工作区全部 TypeScript/JavaScript 文件及每个文件的顶层符号，"
                             "作为工程结构总览。开始任务前先看一遍，确定要改哪些文件；"
                             "单文件结构用 ts_outline，类成员用 ts_members，符号定位用 ts_symbols。";
            def.parameters = {};
            def.required_parameters = {};
            break;
        case Kind::AddClass:
            def.name = "ts_add_class";
            def.description = "在文件末尾新增一个类。code 可选：完整的类声明文本（缩进随意，自动重排）；"
                             "缺省生成空类骨架。新增类优先用它。";
            def.parameters = {file, name, code};
            def.required_parameters = {"file", "name"};
            break;
        case Kind::RenameClass:
            def.name = "ts_rename_class";
            def.description = "重命名类并同步更新项目内所有引用（语义级重命名，比手改安全）。";
            def.parameters = {file, name, newName};
            def.required_parameters = {"file", "name", "new_name"};
            break;
        case Kind::DeleteClass:
            def.name = "ts_delete_class";
            def.description = "删除整个类（含全部成员与函数体）。";
            def.parameters = {file, name};
            def.required_parameters = {"file", "name"};
            break;
        case Kind::AddFunction:
            def.name = "ts_add_function";
            def.description = "新增顶层函数或类方法。scope=\"文件\" 加顶层函数；"
                             "scope=\"文件#类名\" 加方法（自动落在类体内并重排缩进）。"
                             "code 必填：完整的函数/方法声明。新增函数/方法优先用它。";
            def.parameters = {scope, name, code};
            def.required_parameters = {"scope", "name", "code"};
            break;
        case Kind::RenameFunction:
            def.name = "ts_rename_function";
            def.description = "重命名函数/方法并同步更新项目内所有引用（语义级重命名）。"
                             "scope 同 ts_add_function。";
            def.parameters = {scope, name, newName};
            def.required_parameters = {"scope", "name", "new_name"};
            break;
        case Kind::ReplaceFunction:
            def.name = "ts_replace_function";
            def.description = "整体替换函数/方法的声明与函数体。scope 同 ts_add_function，"
                             "code 为新的完整声明文本。改函数优先用它，不要用 modify_file 拼原文。";
            def.parameters = {scope, name, code};
            def.required_parameters = {"scope", "name", "code"};
            break;
        case Kind::DeleteFunction:
            def.name = "ts_delete_function";
            def.description = "删除顶层函数或类方法（整个声明与函数体）。scope 同 ts_add_function。";
            def.parameters = {scope, name};
            def.required_parameters = {"scope", "name"};
            break;
    }

    if (!def.name.empty())
        setToolDefine(def);
}

// ============================================================
// 调用入口
// ============================================================
void JZTsEditTool::call(const ToolCall &params)
{
    const Json::Value &p = params.params;
    switch (m_kind)
    {
        case Kind::Summary:         doSummary(params, p); break;
        case Kind::AddClass:        doAddClass(params, p); break;
        case Kind::AddFunction:     doAddFunction(params, p); break;
        case Kind::ReplaceFunction: doReplaceFunction(params, p); break;
        case Kind::DeleteClass:     doDelete(params, p, true); break;
        case Kind::DeleteFunction:  doDelete(params, p, false); break;
        case Kind::RenameClass:     doRename(params, p, true); break;
        case Kind::RenameFunction:  doRename(params, p, false); break;
    }
}

// ============================================================
// 公共步骤
// ============================================================
void JZTsEditTool::requestOutline(const string &file,
                                  std::function<void(bool, const Json::Value &)> onTree)
{
    Json::Value p;
    p["file"] = file;
    m_server->request(TsMethod::Outline, p,
                      [onTree](bool ok, const string &payload)
                      {
                          Json::Value tree;
                          if (ok)
                          {
                              Json::Reader reader;
                              ok = reader.parse(payload, tree);
                          }
                          onTree(ok, tree);
                      });
}

void JZTsEditTool::parseScope(const string &scope, string &file, string &cls)
{
    size_t hash = scope.find_last_of('#');
    if (hash == string::npos)
    {
        file = scope;
        cls.clear();
    }
    else
    {
        file = scope.substr(0, hash);
        cls = scope.substr(hash + 1);
    }
}

DenyReason JZTsEditTool::checkWrite(const string &abs)
{
    if (!m_host)
        return DenyReason::None; // 测试/无宿主环境跳过
    return checkPathPermission(ToolOperator::Write, abs);
}

void JZTsEditTool::finishEdit(const ToolCall &params, bool ok, const string &msg,
                              DenyReason deny)
{
    finish(params, ok, msg, deny);
}

// ============================================================
// ts_summary
// ============================================================
struct JZTsEditTool::SummaryState
{
    ToolCall params;
    string out;
    vector<pair<string, string>> files; // (相对路径, 绝对路径)
    size_t idx = 0;
};

void JZTsEditTool::summaryStep(std::shared_ptr<SummaryState> st)
{
    if (st->idx >= st->files.size())
    {
        finishEdit(st->params, true, st->out);
        return;
    }
    string rel = st->files[st->idx].first;
    string abs = st->files[st->idx].second;
    st->idx++;
    requestOutline(abs, [this, st, rel](bool ok, const Json::Value &tree)
    {
        st->out += rel + ": ";
        if (ok && tree.isArray())
        {
            vector<string> names;
            for (const auto &s : tree)
                names.push_back(s.get("name", "").asString() + "(" +
                                s.get("kind", "").asString() + ")");
            st->out += str_join(names, ", ");
        }
        else
        {
            st->out += "(解析失败)";
        }
        st->out += "\n";
        summaryStep(st);
    });
}

static void collectTsFiles(const string &dir, const string &relBase,
                           vector<pair<string, string>> &out, int maxFiles)
{
    static const char *skip[] = {"node_modules", "build", "dist", "out", ".git"};
    JZDir d(dir);
    vector<JZFileInfo> list = d.entryInfoList(JZDir::Files | JZDir::Dirs | JZDir::NoDotAndDotDot,
                                              JZDir::SortName);
    for (const auto &info : list)
    {
        if ((int)out.size() >= maxFiles)
            return;
        string rel = relBase.empty() ? info.fileName() : relBase + "/" + info.fileName();
        if (info.isDir())
        {
            bool skipped = false;
            for (const char *s : skip)
                if (info.fileName() == s)
                    skipped = true;
            if (!skipped)
                collectTsFiles(info.filePath(), rel, out, maxFiles);
        }
        else
        {
            string n = str_toLower(info.fileName());
            if (n.size() > 3 &&
                (n.rfind(".ts") == n.size() - 3 || n.rfind(".tsx") == n.size() - 4 ||
                 n.rfind(".js") == n.size() - 3 || n.rfind(".mjs") == n.size() - 4))
                out.emplace_back(rel, info.filePath());
        }
    }
}

void JZTsEditTool::doSummary(const ToolCall &params, const Json::Value &p)
{
    (void)p; // 简化后无参数：固定为工程级总览（file/class 级由 ts_outline/ts_members 覆盖）

    string ws = m_host ? m_host->toolTerminalConfig().zone.workspace : string();
    if (ws.empty())
    {
        finishEdit(params, false, "ts_summary 需要 agent 工作区；单文件结构请用 ts_outline(file)");
        return;
    }
    auto st = std::make_shared<SummaryState>();
    st->params = params;
    collectTsFiles(ws, "", st->files, 50);
    if (st->files.empty())
    {
        finishEdit(params, true, "工作区中没有 TS/JS 文件");
        return;
    }
    st->out = "工作区 TS/JS 文件（前 50 个）及其顶层符号：\n";
    summaryStep(st);
}

// ============================================================
// ts_add_class
// ============================================================
void JZTsEditTool::doAddClass(const ToolCall &params, const Json::Value &p)
{
    string file = p.get("file", "").asString();
    string name = p.get("name", "").asString();
    string code = p.get("code", "").asString();
    if (file.empty() || name.empty())
    {
        finishEdit(params, false, "参数 file / name 不能为空");
        return;
    }
    if (!validIdent(name))
    {
        finishEdit(params, false, "类名 \"" + name + "\" 不是合法标识符");
        return;
    }
    if (code.empty())
        code = "export class " + name + " {\n}";

    string abs = fullPath(file);
    DenyReason deny = checkWrite(abs);
    if (deny != DenyReason::None)
    {
        finishEdit(params, false, denyReasonText(deny), deny);
        return;
    }
    if (!JZFileInfo::exists(abs))
    {
        finishEdit(params, false, "文件不存在: " + file + "（新建文件请用 write_file）",
                   DenyReason::NotFound);
        return;
    }

    string text;
    if (!readFileText(abs, text))
    {
        finishEdit(params, false, "读取文件失败: " + file);
        return;
    }
    bool crlf = false;
    vector<string> lines = splitTextLines(text, crlf);
    while (!lines.empty() && isBlankLine(lines.back()))
        lines.pop_back(); // 收敛末尾空行
    if (!lines.empty())
        lines.push_back("");
    vector<string> add = reindentCode(code, "");
    lines.insert(lines.end(), add.begin(), add.end());

    if (!writeFileText(abs, joinTextLines(lines, crlf) + "\n"))
    {
        finishEdit(params, false, "写入文件失败: " + file);
        return;
    }
    finishEdit(params, true, "已在 " + file + " 末尾添加类 " + name + "（" +
                                 to_string(add.size()) + " 行）");
}

// ============================================================
// ts_add_function / ts_replace_function（共用定位+落点缩进逻辑）
// ============================================================
// 计算插入/替换所需的类内缩进：取类第一个成员行的前导空白
static string memberIndent(const Json::Value &clsNode, const vector<string> &lines)
{
    const Json::Value &children = clsNode.get("children", Json::Value(Json::arrayValue));
    if (children.isArray() && children.size() > 0)
    {
        int ln = children[0].get("line", 0).asInt();
        if (ln >= 1 && ln <= (int)lines.size())
            return leadingWs(lines[ln - 1]);
    }
    return "    ";
}

void JZTsEditTool::doAddFunction(const ToolCall &params, const Json::Value &p)
{
    string scope = p.get("scope", "").asString();
    string name = p.get("name", "").asString();
    string code = p.get("code", "").asString();
    if (scope.empty() || name.empty() || code.empty())
    {
        finishEdit(params, false, "参数 scope / name / code 均不能为空");
        return;
    }
    if (!validIdent(name))
    {
        finishEdit(params, false, "函数名 \"" + name + "\" 不是合法标识符");
        return;
    }

    string file, cls;
    parseScope(scope, file, cls);
    string abs = fullPath(file);
    DenyReason deny = checkWrite(abs);
    if (deny != DenyReason::None)
    {
        finishEdit(params, false, denyReasonText(deny), deny);
        return;
    }

    // 顶层函数：追加到文件末尾（与 add_class 相同落点）
    if (cls.empty())
    {
        string text;
        if (!readFileText(abs, text))
        {
            finishEdit(params, false, "读取文件失败: " + file);
            return;
        }
        bool crlf = false;
        vector<string> lines = splitTextLines(text, crlf);
        while (!lines.empty() && isBlankLine(lines.back()))
            lines.pop_back();
        if (!lines.empty())
            lines.push_back("");
        vector<string> add = reindentCode(code, "");
        lines.insert(lines.end(), add.begin(), add.end());
        if (!writeFileText(abs, joinTextLines(lines, crlf) + "\n"))
        {
            finishEdit(params, false, "写入文件失败: " + file);
            return;
        }
        finishEdit(params, true, "已在 " + file + " 末尾添加函数 " + name);
        return;
    }

    // 类方法：定位类，插入到类体 `}` 前
    requestOutline(abs, [this, params, file, abs, cls, name, code](bool ok, const Json::Value &tree)
    {
        if (!ok)
        {
            finishEdit(params, false, "解析文件符号树失败");
            return;
        }
        string err;
        Json::Value clsNode = locateTarget(tree, cls, string(), err);
        if (clsNode.isNull())
        {
            finishEdit(params, false, err);
            return;
        }
        int endLine = clsNode.get("endLine", 0).asInt();
        string text;
        if (!readFileText(abs, text))
        {
            finishEdit(params, false, "读取文件失败: " + file);
            return;
        }
        bool crlf = false;
        vector<string> lines = splitTextLines(text, crlf);
        int insertIdx = endLine - 1; // 0-based：类体结束 `}` 行之前
        if (insertIdx < 0 || insertIdx > (int)lines.size())
        {
            finishEdit(params, false, "类 " + cls + " 的行范围与文件不符（文件可能已被外部修改）");
            return;
        }
        string indent = memberIndent(clsNode, lines);
        vector<string> add = reindentCode(code, indent);
        lines.insert(lines.begin() + insertIdx, add.begin(), add.end());
        if (!writeFileText(abs, joinTextLines(lines, crlf)))
        {
            finishEdit(params, false, "写入文件失败: " + file);
            return;
        }
        finishEdit(params, true, "已在类 " + cls + " 中添加方法 " + name + "（" +
                                     to_string(add.size()) + " 行）");
    });
}

void JZTsEditTool::doReplaceFunction(const ToolCall &params, const Json::Value &p)
{
    string scope = p.get("scope", "").asString();
    string name = p.get("name", "").asString();
    string code = p.get("code", "").asString();
    if (scope.empty() || name.empty() || code.empty())
    {
        finishEdit(params, false, "参数 scope / name / code 均不能为空");
        return;
    }

    string file, cls;
    parseScope(scope, file, cls);
    string abs = fullPath(file);
    DenyReason deny = checkWrite(abs);
    if (deny != DenyReason::None)
    {
        finishEdit(params, false, denyReasonText(deny), deny);
        return;
    }

    requestOutline(abs, [this, params, file, abs, cls, name, code](bool ok, const Json::Value &tree)
    {
        if (!ok)
        {
            finishEdit(params, false, "解析文件符号树失败");
            return;
        }
        string err;
        Json::Value node = locateTarget(tree, cls, name, err);
        if (node.isNull())
        {
            finishEdit(params, false, err);
            return;
        }
        int line = node.get("line", 0).asInt();
        int endLine = node.get("endLine", 0).asInt();
        if (line < 1 || endLine < line)
        {
            finishEdit(params, false, "符号行范围异常");
            return;
        }
        string text;
        if (!readFileText(abs, text))
        {
            finishEdit(params, false, "读取文件失败: " + file);
            return;
        }
        bool crlf = false;
        vector<string> lines = splitTextLines(text, crlf);
        if (endLine > (int)lines.size())
        {
            finishEdit(params, false, "符号行范围与文件不符（文件可能已被外部修改）");
            return;
        }
        // 落点缩进：顶层函数顶格；类方法用该成员现在的缩进
        string indent = cls.empty() ? string() : leadingWs(lines[line - 1]);
        vector<string> repl = reindentCode(code, indent);

        int begin = line - 1, cnt = endLine - line + 1;
        lines.erase(lines.begin() + begin, lines.begin() + begin + cnt);
        lines.insert(lines.begin() + begin, repl.begin(), repl.end());
        if (!writeFileText(abs, joinTextLines(lines, crlf)))
        {
            finishEdit(params, false, "写入文件失败: " + file);
            return;
        }
        finishEdit(params, true, "已替换 " + (cls.empty() ? "函数 " : cls + "::") + name +
                                     "（原 " + to_string(cnt) + " 行 -> 新 " +
                                     to_string(repl.size()) + " 行）");
    });
}

// ============================================================
// ts_delete_class / ts_delete_function
// ============================================================
void JZTsEditTool::doDelete(const ToolCall &params, const Json::Value &p, bool isClass)
{
    string fileOrScope = isClass ? p.get("file", "").asString() : p.get("scope", "").asString();
    string name = p.get("name", "").asString();
    if (fileOrScope.empty() || name.empty())
    {
        finishEdit(params, false, "参数 " + string(isClass ? "file" : "scope") + " / name 不能为空");
        return;
    }

    // 统一 (cls, fn) 定位参数：class 操作 -> cls=name, fn 空；function 操作 ->
    // cls 来自 scope 的 "#类名"（可空=顶层函数），fn=name
    string file, cls, fn;
    if (isClass)
    {
        file = fileOrScope;
    }
    else
    {
        parseScope(fileOrScope, file, cls);
        fn = name;
    }

    string abs = fullPath(file);
    DenyReason deny = checkWrite(abs);
    if (deny != DenyReason::None)
    {
        finishEdit(params, false, denyReasonText(deny), deny);
        return;
    }

    requestOutline(abs, [this, params, file, abs, cls, fn, name, isClass](bool ok, const Json::Value &tree)
    {
        if (!ok)
        {
            finishEdit(params, false, "解析文件符号树失败");
            return;
        }
        string err;
        Json::Value node = locateTarget(tree, isClass ? name : cls, isClass ? string() : fn, err);
        if (node.isNull())
        {
            finishEdit(params, false, err);
            return;
        }
        int line = node.get("line", 0).asInt();
        int endLine = node.get("endLine", 0).asInt();
        string kindText = node.get("kind", "").asString();
        string text;
        if (!readFileText(abs, text))
        {
            finishEdit(params, false, "读取文件失败: " + file);
            return;
        }
        bool crlf = false;
        vector<string> lines = splitTextLines(text, crlf);
        if (line < 1 || endLine > (int)lines.size() || endLine < line)
        {
            finishEdit(params, false, "符号行范围与文件不符（文件可能已被外部修改）");
            return;
        }
        int begin = line - 1;
        lines.erase(lines.begin() + begin, lines.begin() + begin + (endLine - line + 1));
        // 删除点局部空行收敛：前后都空则去掉一行
        while (begin > 0 && begin < (int)lines.size() &&
               isBlankLine(lines[begin - 1]) && isBlankLine(lines[begin]))
            lines.erase(lines.begin() + begin);
        if (!writeFileText(abs, joinTextLines(lines, crlf)))
        {
            finishEdit(params, false, "写入文件失败: " + file);
            return;
        }
        finishEdit(params, true, string("已删除 ") + (isClass ? "类 " : "函数/方法 ") + name +
                                     "（" + to_string(endLine - line + 1) + " 行，kind=" +
                                     kindText + "）");
    });
}

// ============================================================
// ts_rename_class / ts_rename_function（LSP 语义重命名）
// ============================================================
void JZTsEditTool::doRename(const ToolCall &params, const Json::Value &p, bool isClass)
{
    string fileOrScope = isClass ? p.get("file", "").asString() : p.get("scope", "").asString();
    string name = p.get("name", "").asString();
    string newName = p.get("new_name", "").asString();
    if (fileOrScope.empty() || name.empty() || newName.empty())
    {
        finishEdit(params, false, "参数 " + string(isClass ? "file" : "scope") +
                                      " / name / new_name 均不能为空");
        return;
    }
    if (!validIdent(newName))
    {
        finishEdit(params, false, "新名字 \"" + newName + "\" 不是合法标识符");
        return;
    }

    string file, cls;
    if (isClass)
        file = fileOrScope;
    else
        parseScope(fileOrScope, file, cls);

    string abs = fullPath(file);
    DenyReason deny = checkWrite(abs);
    if (deny != DenyReason::None)
    {
        finishEdit(params, false, denyReasonText(deny), deny);
        return;
    }

    m_server->renameSymbol(abs, name, newName,
                           [this, params, cls, name, newName](bool ok, const string &payload)
    {
        if (!ok)
        {
            finishEdit(params, false, payload);
            return;
        }
        Json::Value files;
        Json::Reader reader;
        if (!reader.parse(payload, files) || !files.isArray())
        {
            finishEdit(params, false, "重命名结果解析失败");
            return;
        }

        // 先检查所有涉及文件可写，任一拒绝则整体不动
        struct FileEdit
        {
            string rel, abs;
            vector<TextEdit1> edits;
            bool crlf = false;
            vector<string> lines;
        };
        vector<FileEdit> todo;
        for (const auto &fe : files)
        {
            FileEdit e;
            e.rel = fe.get("file", "").asString();
            e.abs = fullPath(e.rel);
            DenyReason d = checkWrite(e.abs);
            if (d != DenyReason::None)
            {
                finishEdit(params, false, string(denyReasonText(d)) + ": " + e.rel, d);
                return;
            }
            for (const auto &t : fe.get("edits", Json::Value(Json::arrayValue)))
            {
                TextEdit1 te;
                te.line = t.get("line", 0).asInt();
                te.offset = t.get("offset", 0).asInt();
                te.endLine = t.get("endLine", 0).asInt();
                te.endOffset = t.get("endOffset", 0).asInt();
                te.text = t.get("text", "").asString();
                e.edits.push_back(te);
            }
            todo.push_back(std::move(e));
        }

        int total = 0;
        for (auto &e : todo)
        {
            string text;
            if (!readFileText(e.abs, text))
            {
                finishEdit(params, false, "读取文件失败: " + e.rel);
                return;
            }
            e.lines = splitTextLines(text, e.crlf);
            string err;
            if (!applyRenameEdits(e.lines, e.edits, err))
            {
                finishEdit(params, false, "应用重命名失败(" + e.rel + "): " + err);
                return;
            }
            if (!writeFileText(e.abs, joinTextLines(e.lines, e.crlf)))
            {
                finishEdit(params, false, "写入文件失败: " + e.rel);
                return;
            }
            total += (int)e.edits.size();
        }
        finishEdit(params, true, "已把 \"" + name + "\" 重命名为 \"" + newName + "\"：共 " +
                                     to_string(total) + " 处，涉及 " + to_string(todo.size()) +
                                     " 个文件");
    });
}
