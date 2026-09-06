#include "JZEngineRuntime.h"

#include <cctype>
#include <cstdlib>

#include "JZApplication.h"
#include "JZByteArray.h"
#include "JZFile.h"
#include "JZFileInfo.h"
#include "JZJSEngine.h"
#include "JZObject.h"
#include "JZString.h"
#include "JZThread.h"
#include "json/json.h"

using namespace std;

// 引擎 sigLog -> logHandler 的桥。未捕获异常在 onExceptionOccurred 里以原文
// sigLog 上报，与 runtimeError() 完全相同的行滤掉，交给 lastError() 的翻译版。
class JZEngineRuntime::LogBridge : public JZObject
{
public:
    JZEngineRuntime *rt = nullptr;

    void onLog(const string &text)
    {
        if (text.empty() || !rt)
            return;
        if (rt->m_engine)
        {
            string err = rt->m_engine->runtimeError();
            if (!err.empty() && text == err)
                return;
        }
        if (rt->m_logHandler)
            rt->m_logHandler(text);
    }
};

// 让可能挂起的 promise job / console / timer 落定（模块本体是同步求值的）
static void rtPumpEngine()
{
    JZApplication *app = JZApplication::instance();
    for (int i = 0; i < 40; i++)
    {
        if (app)
            app->processEvents();
        JZThread::msleep(5);
    }
}

// ==================== 文件 / 路径小工具 ====================

static bool rtReadFile(const string &path, string &text)
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

// 工程根下的主入口声明文件（由"打包/发布"步骤 JZCodeAgent::buildProject 生成）：
//     { "entry": "build/main.js" }   // 相对 projectDir 的 ESM 产物路径
// 缺失 / 解析失败返回空串；init() 视作"尚未打包/发布"，直接报错，不猜测入口。
static string rtReadProjectEntry(const string &projectDir)
{
    string text;
    if (!rtReadFile(projectDir + "/project.json", text))
        return "";
    // 去掉 UTF-8 BOM（Json::Reader 不认）
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF &&
        (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
        text.erase(0, 3);

    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(text, root) || !root.isObject() || !root.isMember("entry"))
        return "";
    if (root["entry"].isString())
        return root["entry"].asString();
    return "";
}

static string rtDirOf(const string &path)
{
    size_t p = path.find_last_of("/\\");
    return (p == string::npos) ? string() : path.substr(0, p);
}

static bool rtIsAbsPath(const string &path)
{
    if (!path.empty() && (path[0] == '/' || path[0] == '\\'))
        return true;
    return path.size() >= 2 && path[1] == ':';
}

static string rtNormalizePath(const string &path);

static string rtJoin(const string &dir, const string &name)
{
    if (name.empty())
        return dir;
    if (rtIsAbsPath(name))
        return name;
    if (dir.empty())
        return name;
    return rtNormalizePath(dir + "/" + name);
}

// 统一分隔符为 '/'，消除 "." / ".."。
// 注：源路径可能指向运行根之外（map 的 sources 常写成 "../src/x.ts"），
// 相对路径里越出起始目录的 ".." 会保留下来，不静默夹住。
static string rtNormalizePath(const string &path)
{
    string p = path;
    for (size_t i = 0; i < p.size(); i++)
    {
        if (p[i] == '\\')
            p[i] = '/';
    }

    string prefix;
    if (p.size() >= 2 && p[1] == ':')
    {
        prefix = p.substr(0, 2);        // Windows 盘符
        p = p.substr(2);
    }
    else if (!p.empty() && p[0] == '/')
    {
        prefix = "/";
        p = p.substr(1);
    }

    vector<string> segs;
    size_t pos = 0;
    while (pos <= p.size())
    {
        size_t sep = p.find('/', pos);
        string seg = (sep == string::npos) ? p.substr(pos) : p.substr(pos, sep - pos);
        if (!seg.empty() && seg != ".")
        {
            if (seg == "..")
            {
                if (!segs.empty() && segs.back() != "..")
                    segs.pop_back();
                else if (prefix.empty())
                    segs.push_back("..");
                // 绝对路径上的 ".." 直接丢弃
            }
            else
                segs.push_back(seg);
        }
        if (sep == string::npos)
            break;
        pos = sep + 1;
    }

    string res = prefix;
    for (size_t i = 0; i < segs.size(); i++)
    {
        if (!res.empty() && res.back() != '/')
            res += "/";
        res += segs[i];
    }
    return res;
}

// entry 相对 root 的产物文件；loader 允许省略 ".js" 扩展名，这里补试一次。
// 均不存在返回空串（工程尚未打包/发布）。
static string rtResolveModuleFile(const string &root, const string &entry)
{
    if (entry.empty())
        return string();
    string p1 = rtNormalizePath(rtJoin(root, entry));
    if (JZFileInfo::exists(p1) && JZFileInfo(p1).isFile())
        return p1;
    if (!str_endsWith(entry, ".js"))
    {
        string p2 = rtNormalizePath(rtJoin(root, entry + ".js"));
        if (JZFileInfo::exists(p2) && JZFileInfo(p2).isFile())
            return p2;
    }
    return string();
}

// ==================== Base64 VLQ ====================
// source map 的 mappings 字段：行用 ';' 分隔，段用 ',' 分隔，
// 每段 1/4/5 个 Base64-VLQ 变长整数（低位在前，末位为符号位）。

static int rtBase64Value(char c)
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

bool JZEngineRuntime::decodeVLQ(const string &s, size_t &pos, int &value)
{
    int64_t result = 0;
    int shift = 0;
    bool cont = true;
    size_t start = pos;
    int digits = 0;

    while (pos < s.size())
    {
        int digit = rtBase64Value(s[pos++]);
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

bool JZEngineRuntime::parseMappings(const string &mappings, SourceMap &map)
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

bool JZEngineRuntime::parseMapFile(const string &mapPath, SourceMap &out)
{
    string text;
    if (!rtReadFile(mapPath, text))
        return false;
    // 去掉 UTF-8 BOM（Json::Reader 不认）
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF &&
        (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
    {
        text.erase(0, 3);
    }

    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(text, root) || !root.isObject())
        return false;
    if (!root.isMember("mappings") || !root["mappings"].isString())
        return false;

    out = SourceMap();
    out.mapDir = rtDirOf(rtNormalizePath(mapPath));

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

// ==================== JZEngineRuntime ====================
JZEngineRuntime::JZEngineRuntime()
{
}

JZEngineRuntime::~JZEngineRuntime()
{
    shutdownEngine();
}

// ---- 内嵌 JZJSEngine 会话 ----

bool JZEngineRuntime::initEngine(const string &workDir)
{
    shutdownEngine();   // 每次 initEngine 都是全新的干净会话

    string wd = workDir.empty() ? m_root : rtNormalizePath(workDir);

    JZJSEngine *engine = new JZJSEngine();
    engine->init();
    engine->setWorkDir(wd);
    engine->start();

    LogBridge *bridge = new LogBridge();
    bridge->rt = this;
    connect(&engine->sigLog, bridge, &LogBridge::onLog);

    m_engine = engine;
    m_bridge = bridge;
    m_engineWorkDir = wd;
    m_engineRunning = true;
    m_lastError.clear();
    return true;
}

void JZEngineRuntime::shutdownEngine()
{
    if (m_bridge)
    {
        delete m_bridge;
        m_bridge = nullptr;
    }
    if (m_engine)
    {
        m_engine->deinit();
        delete m_engine;
        m_engine = nullptr;
    }
    m_engineWorkDir.clear();
    m_engineRunning = false;
    m_lastError.clear();
}

bool JZEngineRuntime::engineRunning() const
{
    return m_engineRunning && m_engine != nullptr;
}

bool JZEngineRuntime::runModule(const string &moduleRel)
{
    if (!m_engine)
    {
        m_lastError = "runModule: 引擎未启动，请先 initEngine()";
        return false;
    }

    m_lastError.clear();
    string errBefore = m_engine->runtimeError();
    bool loaded = m_engine->runModuleFile(moduleRel, true);
    rtPumpEngine();     // 让可能挂起的 job / console 输出落定

    string raw = m_engine->runtimeError();
    if (loaded && raw == errBefore)
        return true;

    // 模块顶层抛错会转为 promise 拒绝：runModuleFile 可能仍返回 true，
    // 以 runtimeError 是否变化为准，并整体翻译回 TS
    m_lastError = mapStack(raw.empty() ? errBefore : raw);
    return false;
}

bool JZEngineRuntime::evalScript(const string &script)
{
    if (!m_engine)
    {
        m_lastError = "evalScript: 引擎未启动，请先 initEngine()";
        return false;
    }

    m_lastError.clear();
    string errBefore = m_engine->runtimeError();
    m_engine->eval(script);

    string raw = m_engine->runtimeError();
    if (raw == errBefore)
        return true;

    m_lastError = mapStack(raw);
    return false;
}

const string &JZEngineRuntime::lastError() const
{
    return m_lastError;
}

void JZEngineRuntime::setLogHandler(const LogHandler &handler)
{
    m_logHandler = handler;
}

bool JZEngineRuntime::init(const string &projectDir)
{
    m_root = rtNormalizePath(projectDir);
    while (m_root.size() > 2 && str_endsWith(m_root, "/"))
        str_chop(m_root, 1);
    if (m_root.empty())
    {
        m_lastError = "init: 工程根为空";
        return false;
    }

    // 运行前必须先"打包/发布"（tsc 编译 -> build/ 产物，并生成入口声明 project.json）。
    // 主入口由工程根下的 project.json 声明：读不到 = 还没打包，直接报错不猜测入口；
    // 声明指向的产物文件不存在同样报错，避免把"没构建"误报成源码运行错。
    string moduleRel = rtReadProjectEntry(m_root);
    if (moduleRel.empty())
    {
        m_lastError =
            "init: 工程尚未打包/发布，读不到运行主入口声明 project.json。\n"
            "请先构建工程（tsc -p tsconfig.json，产物在 tsconfig 的 outDir 下）"
            "并生成 project.json 后重试。";
        return false;
    }
    if (rtResolveModuleFile(m_root, moduleRel).empty())
    {
        m_lastError = "init: 入口产物不存在: " + moduleRel +
                      "（工程尚未打包/发布，或 project.json 的 entry 已过时）。";
        return false;
    }

    // 1) 新建干净会话（会自动先关掉旧的）
    if (!initEngine(m_root))
    {
        if (m_lastError.empty())
            m_lastError = "init: 引擎启动失败";
        return false;
    }

    // 2) 以 ESM 方式加载并执行入口模块（入口具名导出绑定到全局）
    if (!runModule(moduleRel))
        return false;                       // lastError 已 mapStack 回 TS

    // 3) 自动调用入口主函数 main()（入口没导出 main 则只执行模块顶层）
    if (!evalScript("if (typeof main === 'function') { main(); }"))
        return false;                       // lastError 已 mapStack 回 TS

    return true;
}

bool JZEngineRuntime::hasPendingJob() const
{
    // 没有会话一律视为"没有待执行任务"（init 失败/已关闭时守卫应立即放行退出）
    return m_engine && m_engine->hasPendingJob();
}

const string &JZEngineRuntime::root() const
{
    return m_root;
}

void JZEngineRuntime::setKeepOriginal(bool keep)
{
    m_keepOriginal = keep;
}

bool JZEngineRuntime::keepOriginal() const
{
    return m_keepOriginal;
}

void JZEngineRuntime::clearCache()
{
    m_maps.clear();
}

const JZEngineRuntime::SourceMap *JZEngineRuntime::findMap(const string &jsFile) const
{
    string js = rtNormalizePath(jsFile);
    if (!m_root.empty() && !rtIsAbsPath(js))
        js = rtNormalizePath(m_root + "/" + js);   // 相对路径按运行根解析

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

string JZEngineRuntime::resolveSource(const SourceMap &map, int srcIndex) const
{
    if (srcIndex < 0 || srcIndex >= (int)map.sources.size())
        return string();

    string src = map.sources[srcIndex];
    if (rtIsAbsPath(src))
        return relativeToRoot(rtNormalizePath(src));

    string base = map.mapDir;
    if (!map.sourceRoot.empty())
        base = rtJoin(map.mapDir, map.sourceRoot);

    // map 的 sources 常写成 "../src/x.ts"，先按 map 所在目录解析，再转成运行根的
    // 相对路径（源路径指向根之外时，保留 ".." 原样输出，不谎报位置）
    return relativeToRoot(rtNormalizePath(rtJoin(base, src)));
}

string JZEngineRuntime::relativeToRoot(const string &path) const
{
    if (m_root.empty())
        return path;
    if (path.size() > m_root.size() && path.compare(0, m_root.size(), m_root) == 0 &&
        path[m_root.size()] == '/')
    {
        return path.substr(m_root.size() + 1);
    }
    return path;
}

bool JZEngineRuntime::mapLocation(const string &jsFile, int line, int column, Location &out) const
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

    // 取"生成列不超过目标列"的最后一段（段按生成列递增）
    const Segment *best = nullptr;
    for (size_t i = 0; i < segs.size(); i++)
    {
        if (segs[i].genColumn <= genColumn)
        {
            if (!best || segs[i].genColumn >= best->genColumn)
                best = &segs[i];
        }
    }
    // 列号落在本行第一个映射之前：退化为该行第一个映射（尽力而为）
    if (!best)
        best = &segs.front();

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
static size_t rtFrameBegin(const string &text, size_t dot)
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
// 冒号前有一段由 rtFrameBegin 取出的路径。
// 返回是否命中；命中时 fileEnd 指向列数字串之后（不含），line/col 为 1-based。
static bool rtFindPosition(const string &text, size_t pos,
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

        size_t b = rtFrameBegin(text, i);
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

string JZEngineRuntime::mapStack(const string &text) const
{
    string out;
    size_t pos = 0;

    while (pos < text.size())
    {
        size_t fileBegin = 0, colon = 0, fileEnd = 0;
        int line = 0, col = 0;
        if (!rtFindPosition(text, pos, fileBegin, colon, fileEnd, line, col))
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
            out += loc.file + ":" + to_string(loc.line) + ":" + to_string(loc.column);
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
