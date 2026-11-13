#include "JZSourceMap.h"

#include <cctype>
#include <cstdlib>

#include "JZByteArray.h"
#include "JZDir.h"
#include "JZFile.h"
#include "JZFileInfo.h"
#include "JZString.h"
#include "json/json.h"

using namespace std;

// ==================== 文件 / 路径小工具 ====================
// 一律走框架这套：JZFile 读盘、JZFileInfo 取目录/存在性、JZDir 判绝对路径与拼接、
// JZFileInfo::weaklyCanonicalPath 做最后的 "." / ".." 规整（不再自己造轮子）。

// 读整个文件到 text；不存在 / 不是普通文件返回 false
static bool smReadFile(const string &path, string &text)
{
    JZFileInfo info(path);
    if (!info.exists() || !info.isFile())
        return false;

    JZFile file;
    if (!file.open(path, "rb"))
        return false;

    JZByteArray data = file.readAll();
    text.assign(data.data(), data.size());
    file.close();
    return true;
}

// 取所在目录（无分隔符时返回原串）
static string smDirOf(const string &path)
{
    return JZFileInfo(path).path();
}

// 规整成绝对路径：相对量按 base 解析（base 为空则按当前工作目录），
// 再统一分隔符为 '/' 并消除 "." / ".."。
// 注：JZFileInfo::weaklyCanonicalPath 内部 assert 入参必须是绝对路径，所以先用
// JZDir::absoluteFilePath 把相对量挂到 base（或当前目录）上；path 本身是绝对路径
// 时 absoluteFilePath 原样返回，只做规整。
static string smAbsPath(const string &base, const string &path)
{
    if (path.empty())
        return string();
    return JZFileInfo::weaklyCanonicalPath(JZDir(base).absoluteFilePath(path));
}

// ==================== Base64 VLQ ====================
// source map 的 mappings 字段：行用 ';' 分隔，段用 ',' 分隔，
// 每段 1/4/5 个 Base64-VLQ 变长整数（低位在前，末位为符号位）。

static int smBase64Value(char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

bool JZSourceMap::decodeVLQ(const string &s, size_t &pos, int &value)
{
    int64_t result = 0;
    int shift = 0;
    bool cont = true;
    size_t start = pos;
    int digits = 0;

    while (pos < s.size())
    {
        int digit = smBase64Value(s[pos++]);
        if (digit < 0)
        {
            pos = start;
            return false;   // 非法字符
        }
        digits++;
        cont = (digit & 32) != 0;
        result += (int64_t)(digit & 31) << shift;
        shift += 5;
        if (!cont)
            break;
    }

    if (digits == 0 || cont)
    {
        pos = start;
        return false;       // 空字段或被截断
    }

    bool negative = (result & 1) != 0;
    result >>= 1;
    value = negative ? -(int)result : (int)result;
    return true;
}

bool JZSourceMap::parseMappings(const string &mappings, SourceMap &map)
{
    int srcIndex = 0;
    int srcLine = 0;
    int srcColumn = 0;
    int nameIndex = 0;
    size_t pos = 0;

    while (pos <= mappings.size())
    {
        // 行之间只用 ';' 分隔；',' 是同一行内段的分隔，在下面的内层循环里处理
        size_t sep = mappings.find(';', pos);
        string lineText = (sep == string::npos) ? mappings.substr(pos)
                                                : mappings.substr(pos, sep - pos);

        vector<Segment> segs;
        int genColumn = 0;
        size_t lp = 0;
        while (lp < lineText.size())
        {
            int v = 0;
            if (!decodeVLQ(lineText, lp, v))
                return false;           // 第一个字段（生成列）必须存在
            genColumn += v;

            Segment seg;
            seg.genColumn = genColumn;

            if (lp < lineText.size() && lineText[lp] != ',')
            {
                if (!decodeVLQ(lineText, lp, v))
                    return false;
                srcIndex += v;

                if (lp < lineText.size() && lineText[lp] != ',')
                {
                    if (!decodeVLQ(lineText, lp, v))
                        return false;
                    srcLine += v;

                    if (lp < lineText.size() && lineText[lp] != ',')
                    {
                        if (!decodeVLQ(lineText, lp, v))
                            return false;
                        srcColumn += v;

                        if (lp < lineText.size() && lineText[lp] != ',')
                        {
                            if (!decodeVLQ(lineText, lp, v))
                                return false;
                            nameIndex += v;
                            seg.nameIndex = nameIndex;
                        }
                    }
                }
            }

            seg.srcIndex = srcIndex;
            seg.srcLine = srcLine;
            seg.srcColumn = srcColumn;
            segs.push_back(seg);

            if (lp < lineText.size() && lineText[lp] == ',')
                lp++;
        }

        map.lines.push_back(segs);

        if (sep == string::npos)
            break;
        pos = sep + 1;
    }

    return true;
}

bool JZSourceMap::parseMapFile(const string &mapPath, SourceMap &out)
{
    string text;
    if (!smReadFile(mapPath, text))
        return false;
    // 去掉 UTF-8 BOM（Json::Reader 不认）
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF &&
        (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
    {
        text.erase(0, 3);
    }
    // 去掉规范允许的 XSSI 前缀 ")]}'"（防 .map 被当 JS 直接 include）
    if (text.size() >= 4 && text.compare(0, 4, ")]}'") == 0)
    {
        size_t nl = text.find('\n');
        text.erase(0, nl == string::npos ? text.size() : nl + 1);
    }

    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(text, root) || !root.isObject())
        return false;
    if (!root.isMember("mappings") || !root["mappings"].isString())
        return false;

    out = SourceMap();
    out.mapDir = smDirOf(smAbsPath(string(), mapPath));

    if (root.isMember("sourceRoot") && root["sourceRoot"].isString())
        out.sourceRoot = root["sourceRoot"].asString();
    if (root.isMember("sources") && root["sources"].isArray())
    {
        for (Json::ArrayIndex i = 0; i < root["sources"].size(); i++)
            out.sources.push_back(root["sources"][i].asString());
    }
    if (root.isMember("names") && root["names"].isArray())
    {
        for (Json::ArrayIndex i = 0; i < root["names"].size(); i++)
            out.names.push_back(root["names"][i].asString());
    }

    if (!parseMappings(root["mappings"].asString(), out))
    {
        out = SourceMap();
        return false;
    }

    out.valid = true;
    return true;
}

// ==================== JZSourceMap ====================
JZSourceMap::JZSourceMap()
{
}

void JZSourceMap::setRoot(const string &root)
{
    m_root = root;
}

const string &JZSourceMap::root() const
{
    return m_root;
}

void JZSourceMap::setKeepOriginal(bool keep)
{
    m_keepOriginal = keep;
}

bool JZSourceMap::keepOriginal() const
{
    return m_keepOriginal;
}

void JZSourceMap::clearCache()
{
    m_maps.clear();
}

const JZSourceMap::SourceMap *JZSourceMap::findMap(const string &jsFile) const
{
    // 相对路径按运行根解析（m_root 为空则按当前工作目录），再规整成绝对路径
    string js = smAbsPath(m_root, jsFile);

    auto it = m_maps.find(js);
    if (it != m_maps.end())
        return it->second.valid ? &it->second : nullptr;

    // 约定：map 与产物同目录同名 + ".map"（tsc 默认行为）
    SourceMap map;
    parseMapFile(js + ".map", map);

    // 引擎模块 loader 对"省略扩展名"的 import 只编译出无后缀的模块名
    // （栈帧如 build/lib:5:9，不带 ".js"），此时 map 实际落在 <名>.js.map
    if (!map.valid && !str_endsWith(js, ".js"))
        parseMapFile(js + ".js.map", map);

    auto res = m_maps.insert(make_pair(js, map));
    return res.first->second.valid ? &res.first->second : nullptr;
}

string JZSourceMap::resolveSource(const SourceMap &map, int srcIndex) const
{
    if (srcIndex < 0 || srcIndex >= (int)map.sources.size())
        return string();

    string src = map.sources[srcIndex];

    string base = map.mapDir;
    if (!map.sourceRoot.empty())
        base = smAbsPath(map.mapDir, map.sourceRoot);

    // map 的 sources 常写成 "../src/x.ts"，先按 map 所在目录（含 sourceRoot）解析成
    // 规范的绝对路径；sources 写成绝对路径时 smAbsPath 原样规整、忽略 base。
    // 到此解析就结束了——不再把绝对路径压回相对形态（那是显示层的事）。
    return smAbsPath(base, src);
}

bool JZSourceMap::mapLocation(const string &jsFile, int line, int column, Location &out) const
{
    const SourceMap *map = findMap(jsFile);
    if (!map)
        return false;

    int lineIndex = line - 1;       // 栈里行号 1-based，map 里 0-based
    if (lineIndex < 0 || lineIndex >= (int)map->lines.size())
        return false;

    const vector<Segment> &segs = map->lines[lineIndex];
    if (segs.empty())
        return false;

    int genColumn = column - 1;     // 栈里列号 1-based（V8 / QuickJS 一致），map 里 0-based
    if (genColumn < 0)
        genColumn = 0;

    // 取"生成列不超过目标列"的最后一段（段按生成列递增）。
    // 与官方 source-map 的 GREATEST_LOWER_BOUND 一致：取不到（列号落在本行第一个
    // 映射之前）就老实说映射不到，不要退化成本行第一个映射——那会报错位置。
    const Segment *best = nullptr;
    for (size_t i = 0; i < segs.size(); i++)
    {
        if (segs[i].genColumn <= genColumn)
        {
            if (!best || segs[i].genColumn >= best->genColumn)
                best = &segs[i];
        }
    }
    if (!best)
        return false;

    string file = resolveSource(*map, best->srcIndex);
    if (file.empty())
        return false;

    out.file = file;
    out.line = best->srcLine + 1;
    out.column = best->srcColumn + 1;
    out.name.clear();
    if (best->nameIndex >= 0 && best->nameIndex < (int)map->names.size())
        out.name = map->names[best->nameIndex];
    return true;
}

// 从 ".js:" 的位置往回取完整路径（遇到空白 / 括号 / 引号等分隔符为止）
static size_t smFrameBegin(const string &text, size_t dot)
{
    size_t begin = dot;
    while (begin > 0)
    {
        char c = text[begin - 1];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
            c == '(' || c == ')' || c == '[' || c == ']' ||
            c == '<' || c == '>' || c == '"' || c == '\'' || c == '`')
            break;
        begin--;
    }
    return begin;
}

// 找下一处 "<文件>:<行>:<列>" 形态的位置标记。
// 定位策略：栈帧形如 "    at boom (build/lib:5:9)" 或 "    at <module>
// (build/main.js:3:1)"。文件可能不带 ".js"（引擎模块 loader 对省略扩展名的
// import 生成无后缀模块名），因此不能只认 ".js:"。判定规则：冒号后紧跟
// 行数字 ':' 列数字（Windows 盘符 "C:"、"://" 这类冒号后不是数字，自动跳过），
// 冒号前有一段由 smFrameBegin 取出的路径。
// 返回是否命中；命中时 fileEnd 指向列数字串之后（不含），line/col 为 1-based。
static bool smFindPosition(const string &text, size_t pos,
                           size_t &fileBegin, size_t &colon,
                           size_t &fileEnd, int &line, int &col)
{
    size_t i = pos;
    while (i < text.size())
    {
        if (text[i] != ':')
        {
            i++;
            continue;
        }

        // ':' 后必须紧跟 行数字 ':' 列数字
        size_t p = i + 1;
        size_t lineBegin = p;
        while (p < text.size() && isdigit((unsigned char)text[p]))
            p++;
        if (p == lineBegin || p >= text.size() || text[p] != ':')
        {
            i = (p > i + 1) ? p : i + 1;
            continue;
        }

        p++;
        size_t colBegin = p;
        while (p < text.size() && isdigit((unsigned char)text[p]))
            p++;
        if (p == colBegin)
        {
            i = (p > i + 1) ? p : i + 1;
            continue;
        }

        size_t b = smFrameBegin(text, i);
        if (i == b) // ':' 前必须有一段文件路径
        {
            i = p;
            continue;
        }

        fileBegin = b;
        colon = i;
        fileEnd = p;
        line = atoi(text.substr(i + 1, colBegin - i - 2).c_str());
        col = atoi(text.substr(colBegin, p - colBegin).c_str());
        return true;
    }
    return false;
}

string JZSourceMap::mapStack(const string &text, const PathMapper &mapPath) const
{
    string out;
    size_t pos = 0;

    while (pos < text.size())
    {
        size_t fileBegin = 0, colon = 0, fileEnd = 0;
        int line = 0, col = 0;
        if (!smFindPosition(text, pos, fileBegin, colon, fileEnd, line, col))
        {
            out.append(text, pos, string::npos);
            break;
        }

        // 位置之前的内容原样带上
        out.append(text, pos, fileBegin - pos);

        string file = text.substr(fileBegin, colon - fileBegin);
        Location loc;
        if (mapLocation(file, line, col, loc))
        {
            // loc.file 是解析出的绝对路径；显示成什么样由调用方决定
            string shown = mapPath ? mapPath(loc.file) : loc.file;
            out += shown + ":" + to_string(loc.line) + ":" + to_string(loc.column);
            if (m_keepOriginal)
                out += " (js: " + text.substr(fileBegin, fileEnd - fileBegin) + ")";
        }
        else
        {
            // 映射不到：保持原样
            out.append(text, fileBegin, fileEnd - fileBegin);
        }
        pos = fileEnd;
    }

    return out;
}
