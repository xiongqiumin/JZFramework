#include "JZTsLangServer.h"

#include <algorithm>
#include <climits>
#include <cstring>

#ifdef _WIN32
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#endif

#include "JZFile.h"
#include "JZFileInfo.h"
#include "JZPlatform.h"
#include "JZStd.h"

using namespace std;

// ==================== 构造 / 析构 ====================
JZTsLangServer::JZTsLangServer(JZObject *parent)
    : JZObject(parent),
      sigLog(this)
{
    // 看门狗：周期检查在途请求是否超时（超时 → 失败并重启语言服务）
    m_watchdogTimer = startTimer(1000);
}

JZTsLangServer::~JZTsLangServer()
{
    shutdown();
}

void JZTsLangServer::setTsgoPath(const string &tsc)
{
    m_tsc = tsc;
}

bool JZTsLangServer::isAvailable() const
{
    return !m_tsc.empty() && JZFileInfo::exists(m_tsc);
}

void JZTsLangServer::setRoot(const string &root)
{
    m_root = root;
}

void JZTsLangServer::setRequestTimeout(int ms)
{
    m_requestTimeoutMs = ms;
}

void JZTsLangServer::killProcessForTest()
{
    if (!m_proc)
        return;
    // 注意：JZProcess::killProcess 在 Windows 上会关闭进程句柄，导致
    // onReadTimer 不再检测到退出、sigFinished 不会发射。因此主动走异常自愈。
    // （真实崩溃由 sigFinished -> onFinished 触发，逻辑一致。）
    m_proc->killProcess();
    handleProcessGone("killed by test (模拟崩溃)");
}

void JZTsLangServer::shutdown()
{
    m_alive = false;
    m_ready = false;
    if (m_watchdogTimer)
    {
        killTimer(m_watchdogTimer);
        m_watchdogTimer = 0;
    }
    if (m_restartTimer)
    {
        killTimer(m_restartTimer);
        m_restartTimer = 0;
    }
    m_restartPending = false;
    m_pending.clear();
    m_queue.clear();
    m_docs.clear();
    if (m_proc)
    {
        m_proc->killProcess();
        m_proc->deleteLater();
        m_proc = nullptr;
    }
}

// ==================== 启动与握手 ====================
void JZTsLangServer::ensureStarted()
{
    if (m_proc || !isAvailable())
        return;

    m_alive = true;
    m_proc = new JZProcess(this);
    connect(&m_proc->sigStarted, this, &JZTsLangServer::onStarted);
    connect(&m_proc->sigFinished, this, &JZTsLangServer::onFinished);
    connect(&m_proc->sigErrorOccurred, this, &JZTsLangServer::onErrorOccurred);
    connect(&m_proc->sigReadyReadStandardOutput, this, &JZTsLangServer::onStdOutReady);
    connect(&m_proc->sigReadyReadStandardError, this, &JZTsLangServer::onStdErrReady);
    m_proc->start(m_tsc, {"--lsp", "--stdio"});
}

void JZTsLangServer::onStarted()
{
    // 构造 LSP initialize 参数（rootUri / workspaceFolders / 客户端能力）
    Json::Value params;
    if (!m_root.empty())
    {
        string rootUri = fileToUri(m_root);
        params["rootUri"] = rootUri;
        Json::Value wf(Json::arrayValue);
        Json::Value w;
        w["uri"] = rootUri;
        w["name"] = m_root;
        wf.append(w);
        params["workspaceFolders"] = wf;
    }

    Json::Value &caps = params["capabilities"];
    caps["textDocument"]["synchronization"]["dynamicRegistration"] = false;
    caps["textDocument"]["synchronization"]["didSave"] = true;
    caps["textDocument"]["hover"]["dynamicRegistration"] = false;
    caps["textDocument"]["hover"]["contentFormat"].append("markdown");
    caps["textDocument"]["completion"]["dynamicRegistration"] = false;
    caps["textDocument"]["completion"]["completionItem"]["snippetSupport"] = false;
    caps["textDocument"]["completion"]["completionItem"]["documentationFormat"].append("markdown");
    caps["textDocument"]["completion"]["contextSupport"] = true;
    caps["textDocument"]["definition"]["dynamicRegistration"] = false;
    caps["textDocument"]["definition"]["linkSupport"] = true;
    caps["textDocument"]["typeDefinition"]["dynamicRegistration"] = false;
    caps["textDocument"]["implementation"]["dynamicRegistration"] = false;
    caps["textDocument"]["references"]["dynamicRegistration"] = false;
    caps["textDocument"]["documentHighlight"]["dynamicRegistration"] = false;
    caps["textDocument"]["documentSymbol"]["dynamicRegistration"] = false;
    caps["textDocument"]["documentSymbol"]["hierarchicalDocumentSymbolSupport"] = true;
    caps["textDocument"]["rename"]["dynamicRegistration"] = false;
    caps["textDocument"]["rename"]["prepareSupport"] = true;
    caps["textDocument"]["diagnostic"]["dynamicRegistration"] = false;
    caps["textDocument"]["publishDiagnostics"]["relatedInformation"] = true;
    caps["workspace"]["workspaceFolders"] = true;
    caps["workspace"]["didChangeConfiguration"]["dynamicRegistration"] = false;

    params["locale"] = "en";

    // TSGO 的 LSP 要求 initialize 必须带 processId（LSP 规范里它是可选的，但 TSGO 强制校验）
#ifdef _WIN32
    params["processId"] = (int)GetCurrentProcessId();
#else
    params["processId"] = (int)getpid();
#endif

    callLsp("initialize", params,
            [this](const Json::Value &result)
            {
                m_ready = true;
                m_restartBackoff = 0;   // 成功握手，清零连续失败退避
                // 收到 initialize 响应后必须发 initialized 通知
                sendNotification("initialized", Json::Value(Json::objectValue));
                string info = "tsgo 语言服务已就绪";
                if (result.isMember("serverInfo"))
                    info += ": " + result["serverInfo"].get("name", "").asString() + " " +
                            result["serverInfo"].get("version", "").asString();
                sigLog.emit(info);
                // 冲刷握手期间排队的查询
                while (!m_queue.empty())
                {
                    QueuedReq q = std::move(m_queue.front());
                    m_queue.pop_front();
                    dispatch(q.method, q.params, q.cb);
                }
            },
            [this](const string &e)
            {
                sigLog.emit("tsgo 初始化失败: " + e);
            });
}

void JZTsLangServer::onFinished(int exitCode, JZProcess::ExitStatus status)
{
    // 进程退出：失败在途请求并重启（排队请求保留，待重启握手后重新派发）
    handleProcessGone("tsgo 语言服务进程已退出 (code=" + to_string(exitCode) + ")");
}

void JZTsLangServer::onErrorOccurred(JZProcess::ProcessError err)
{
    sigLog.emit("tsgo 语言服务进程错误: " + to_string((int)err));
    if (m_proc)
        m_proc->killProcess();
    // killProcess 通常会触发 onFinished -> handleProcessGone；若未触发（如启动失败）
    // 也主动走一遍重启流程（幂等，重复调用安全）。
    handleProcessGone("tsgo 语言服务进程错误: " + to_string((int)err));
}

void JZTsLangServer::failAllPending(const string &reason)
{
    for (auto &kv : m_pending)
        if (kv.second.onError)
            kv.second.onError(reason);
    m_pending.clear();
}

void JZTsLangServer::handleProcessGone(const string &reason)
{
    failAllPending(reason);
    m_docs.clear();
    m_buf.clear();
    m_ready = false;
    if (m_proc)
    {
        m_proc->deleteLater();
        m_proc = nullptr;
    }
    // 排队中（握手未完成）的请求保留，待重启握手后由 onStarted 重新派发
    sigLog.emit("tsgo 语言服务异常，准备重启: " + reason);
    scheduleRestart();
}

void JZTsLangServer::scheduleRestart()
{
    if (!m_alive)
        return;
    if (m_restartPending)
        return; // 已有待触发的重启定时器
    m_restartPending = true;
    // 退避累加（上一次成功握手会清零），避免崩溃循环打满 CPU
    int64_t interval = m_restartBackoff > 0 ? m_restartBackoff : 300;
    m_restartTimer = startTimer((int)interval);
}

void JZTsLangServer::restartNow()
{
    if (m_restartTimer)
    {
        killTimer(m_restartTimer);
        m_restartTimer = 0;
    }
    m_restartPending = false;
    if (!m_alive)
        return;
    m_restartBackoff = std::min(m_restartBackoff + 300, (int64_t)5000);
    if (m_proc)
        return; // 已有进程在运行（可能由新请求触发），无需再启
    ensureStarted();
}

void JZTsLangServer::checkTimeouts()
{
    if (m_requestTimeoutMs <= 0 || m_pending.empty())
        return;
    int64_t now = JZPlatform::getTickCountMs();
    for (const auto &kv : m_pending)
    {
        if (now - kv.second.enqueueMs >= m_requestTimeoutMs)
        {
            handleProcessGone("请求超时: " + to_string(m_requestTimeoutMs / 1000) +
                              "s 内无回复，重启语言服务");
            return;
        }
    }
}

void JZTsLangServer::timerEvent(JZTimerEvent *event)
{
    int id = event->id();
    if (id == m_watchdogTimer)
        checkTimeouts();
    else if (id == m_restartTimer)
        restartNow();
}

void JZTsLangServer::onStdErrReady()
{
    if (!m_proc)
        return;
    // 仅排空，避免 Windows 管道缓冲写满导致子进程阻塞；内容原样上抛到日志
    JZByteArray data = m_proc->readAllStandardError();
    string s = str_fromUtf8(data);
    if (!s.empty())
        sigLog.emit("[tsgo stderr] " + s);
}

// ==================== LSP 帧解析 ====================
static size_t parseContentLength(const string &header)
{
    // header 形如 "Content-Length: 123\r\n..." 或 "...\n..."（行尾可能是 \r\n 或 \n）。
    // 统一按 \n 切行，并剥掉行尾可能的 \r。
    size_t pos = 0;
    while (pos < header.size())
    {
        size_t eol = header.find('\n', pos);
        string line = (eol == string::npos) ? header.substr(pos) : header.substr(pos, eol - pos);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        size_t colon = line.find(':');
        if (colon != string::npos)
        {
            string key = str_trimmed(line.substr(0, colon));
            string val = str_trimmed(line.substr(colon + 1));
            if (str_toLower(key) == "content-length")
            {
                try
                {
                    return (size_t)stoul(val);
                }
                catch (...)
                {
                    return 0;
                }
            }
        }
        if (eol == string::npos)
            break;
        pos = eol + 1;
    }
    return 0;
}

void JZTsLangServer::onStdOutReady()
{
    if (!m_proc)
        return;
    JZByteArray data = m_proc->readAllStandardOutput();
    m_buf.append(data.data(), data.size());

    // 反复从缓冲中切出完整的 "Content-Length: N\r\n\r\n{body}" 帧。
    // 注意：TSGO 经 JZProcess 管道读回的 stdout 行尾被剥成 LF（\n\n），
    // 因此分隔符既可能是 \r\n\r\n 也可能是 \n\n，两种都要兼容。
    while (true)
    {
        size_t he = m_buf.find("\r\n\r\n");
        size_t sepLen = 4;
        if (he == string::npos)
        {
            he = m_buf.find("\n\n");
            sepLen = 2;
        }
        if (he == string::npos)
            break;
        size_t cl = parseContentLength(m_buf.substr(0, he));
        if (cl == 0)
            break; // 帧头异常，停止解析避免死循环
        size_t bodyStart = he + sepLen;
        if (m_buf.size() < bodyStart + cl)
            break; // 帧体未收全，等下次
        string body = m_buf.substr(bodyStart, cl);
        m_buf = m_buf.substr(bodyStart + cl);

        Json::Reader reader;
        Json::Value msg;
        if (reader.parse(body, msg))
            handleMessage(msg);
        else
            break;
    }
}

void JZTsLangServer::handleMessage(const Json::Value &msg)
{
    if (msg.isMember("method"))
    {
        if (msg.isMember("id"))
        {
            // 服务器 -> 客户端请求（如 client/registerCapability）。
            // 我们无需真正处理注册，直接回成功结果，否则服务器会一直等待而阻塞语言服务。
            Json::Value resp;
            resp["id"] = msg["id"];
            resp["result"] = Json::Value(Json::objectValue);
            sendFrame(resp);
            return;
        }
        handleNotification(msg);
        return;
    }

    if (msg.isMember("id") && (msg.isMember("result") || msg.isMember("error")))
    {
        int id = msg["id"].asInt();
        auto it = m_pending.find(id);
        if (it == m_pending.end())
            return;
        LspReq req = it->second;
        m_pending.erase(it);
        if (msg.isMember("error"))
        {
            string e = msg["error"].isMember("message")
                           ? msg["error"]["message"].asString()
                           : "LSP error";
            if (req.onError)
                req.onError(e);
        }
        else if (req.onResult)
        {
            req.onResult(msg["result"]);
        }
    }
}

void JZTsLangServer::handleNotification(const Json::Value &msg)
{
    string method = msg.get("method", "").asString();
    if (method == "window/logMessage" || method == "window/showMessage")
    {
        string m = msg.isMember("params") && msg["params"].isMember("message")
                       ? msg["params"]["message"].asString()
                       : string();
        if (!m.empty())
            sigLog.emit("[tsgo] " + m);
    }
    // textDocument/publishDiagnostics / telemetry / progress / $/ 等：忽略
}

// ==================== LSP 收发 ====================
void JZTsLangServer::sendFrame(const Json::Value &msg)
{
    if (!m_proc)
        return;
    Json::Value m = msg;
    m["jsonrpc"] = "2.0";
    Json::FastWriter writer;
    string body = writer.write(m);
    while (!body.empty() && (body.back() == '\n' || body.back() == '\r'))
        body.pop_back();

    string frame = "Content-Length: " + to_string(body.size()) + "\r\n\r\n" + body;
    m_proc->write(str_toUtf8(frame));
}

void JZTsLangServer::sendNotification(const string &method, const Json::Value &params)
{
    Json::Value n;
    n["method"] = method;
    n["params"] = params;
    sendFrame(n);
}

void JZTsLangServer::callLsp(const string &method, const Json::Value &params,
                             function<void(const Json::Value &)> onResult,
                             function<void(const string &)> onError)
{
    int id = m_nextId++;
    Json::Value req;
    req["id"] = id;
    req["method"] = method;
    req["params"] = params;

    LspReq r;
    r.onResult = onResult;
    r.onError = onError;
    r.enqueueMs = JZPlatform::getTickCountMs();
    m_pending[id] = std::move(r);
    sendFrame(req);
}

// ==================== 文档同步 ====================
string JZTsLangServer::readFileUtf8(const string &file)
{
    JZFile f;
    if (!f.open(file, "rb"))
        return string();
    JZByteArray data = f.readAll();
    f.close();
    return str_fromUtf8(data);
}

string JZTsLangServer::langIdOf(const string &file)
{
    string ext = file;
    size_t dot = ext.find_last_of(".");
    if (dot != string::npos)
        ext = str_toLower(ext.substr(dot));
    if (ext == ".ts" || ext == ".tsx")
        return "typescript";
    if (ext == ".js" || ext == ".jsx" || ext == ".mjs" || ext == ".cjs")
        return "javascript";
    if (ext == ".json")
        return "json";
    return "typescript";
}

string JZTsLangServer::ensureOpen(const string &file)
{
    string uri = fileToUri(file);
    auto it = m_docs.find(uri);
    string text = readFileUtf8(file);

    if (it == m_docs.end())
    {
        OpenDoc d;
        d.version = 1;
        d.text = text;
        Json::Value params;
        params["textDocument"]["uri"] = uri;
        params["textDocument"]["languageId"] = langIdOf(file);
        params["textDocument"]["version"] = 1;
        params["textDocument"]["text"] = text;
        sendNotification("textDocument/didOpen", params);
        m_docs[uri] = d;
    }
    else if (it->second.text != text)
    {
        int v = ++it->second.version;
        it->second.text = text;
        Json::Value params;
        params["textDocument"]["uri"] = uri;
        params["textDocument"]["version"] = v;
        Json::Value change;
        change["text"] = text;
        params["contentChanges"].append(change);
        sendNotification("textDocument/didChange", params);
    }
    return uri;
}

// ==================== URI / 路径 ====================
static string encodeUriPath(const string &s)
{
    static const char *keep = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~/:";
    static const char *hex = "0123456789ABCDEF";
    string out;
    for (unsigned char c : s)
    {
        if (strchr(keep, (char)c))
            out += (char)c;
        else
        {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
    }
    return out;
}

string JZTsLangServer::fileToUri(const string &file)
{
    string p = file;
    for (char &c : p)
        if (c == '\\')
            c = '/';
    if (p.size() >= 2 && p[1] == ':')
        return "file:///" + encodeUriPath(p); // Windows 盘符: file:///C:/...
    return "file://" + encodeUriPath(p);
}

static int hexVal(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static string percentDecode(const string &s)
{
    string out;
    for (size_t i = 0; i < s.size(); i++)
    {
        if (s[i] == '%' && i + 2 < s.size())
        {
            int hi = hexVal(s[i + 1]);
            int lo = hexVal(s[i + 2]);
            if (hi >= 0 && lo >= 0)
            {
                out += (char)((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

string JZTsLangServer::uriToFile(const string &uri)
{
    string s = uri;
    if (s.rfind("file:///", 0) == 0)
        s = s.substr(8);
    else if (s.rfind("file://", 0) == 0)
        s = s.substr(7);
    s = percentDecode(s); // 服务器返回的定义 URI 可能是百分号编码（如 c%3A/...）
    for (char &c : s)
        if (c == '/')
            c = '\\';
    return s;
}

// ==================== 结果归一化辅助 ====================
static int jInt(const Json::Value &v, const char *f, int def)
{
    return v.isMember(f) ? v[f].asInt() : def;
}

Json::Value JZTsLangServer::locToObj(const Json::Value &loc)
{
    Json::Value o;
    string uri = loc.isMember("targetUri") ? loc["targetUri"].asString()
                                            : loc.get("uri", "").asString();
    Json::Value range = loc.isMember("targetRange") ? loc["targetRange"] : loc.get("range", Json::Value());
    o["file"] = uriToFile(uri);
    if (range.isObject())
    {
        o["line"] = jInt(range["start"], "line", 0) + 1;
        o["offset"] = jInt(range["start"], "character", 0) + 1;
        o["endLine"] = jInt(range["end"], "line", 0) + 1;
        o["endOffset"] = jInt(range["end"], "character", 0) + 1;
    }
    return o;
}

Json::Value JZTsLangServer::normalizeLocations(const Json::Value &r)
{
    Json::Value out(Json::arrayValue);
    if (r.isNull())
        return out;
    if (r.isArray())
    {
        for (const auto &l : r)
            out.append(locToObj(l));
    }
    else
    {
        out.append(locToObj(r));
    }
    return out;
}

Json::Value JZTsLangServer::firstLocation(const Json::Value &def)
{
    if (def.isNull())
        return Json::Value();
    if (def.isArray())
        return def.size() > 0 ? def[0] : Json::Value();
    return def;
}

vector<string> JZTsLangServer::splitLines(const string &text)
{
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
    {
        if (cur.back() == '\r')
            cur.pop_back();
        lines.push_back(cur);
    }
    return lines;
}

Json::Value JZTsLangServer::readFileRange(const string &file, const Json::Value &range)
{
    string text = readFileUtf8(file);
    if (text.empty())
        return Json::Value(string());

    vector<string> lines = splitLines(text);

    int sl = range.isObject() ? jInt(range["start"], "line", 0) : 0;
    int el = range.isObject() ? jInt(range["end"], "line", 0) : 0;
    if (sl < 0)
        sl = 0;
    if (el < 0)
        el = 0;
    if (sl > el)
        swap(sl, el);
    if (el >= (int)lines.size())
        el = (int)lines.size() - 1;

    string result;
    for (int i = sl; i <= el; i++)
    {
        if (i > sl)
            result += "\n";
        result += lines[i];
    }
    return Json::Value(result);
}

// LSP SymbolKind(1~26) -> 可读文本
string JZTsLangServer::symbolKindName(int kind)
{
    switch (kind)
    {
        case 1: return "file";
        case 2: return "module";
        case 3: return "namespace";
        case 4: return "package";
        case 5: return "class";
        case 6: return "method";
        case 7: return "property";
        case 8: return "field";
        case 9: return "constructor";
        case 10: return "enum";
        case 11: return "interface";
        case 12: return "function";
        case 13: return "variable";
        case 14: return "constant";
        case 15: return "string";
        case 16: return "number";
        case 17: return "boolean";
        case 18: return "array";
        case 19: return "object";
        case 20: return "key";
        case 21: return "null";
        case 22: return "enumMember";
        case 23: return "struct";
        case 24: return "event";
        case 25: return "operator";
        case 26: return "typeParameter";
        default: return "unknown";
    }
}

// documentSymbol 节点 -> 摘要：只留大模型需要的字段，行号归一化为 1-based
Json::Value JZTsLangServer::symbolToJson(const Json::Value &sym)
{
    Json::Value o;
    o["name"] = sym.get("name", "").asString();
    o["kind"] = symbolKindName(sym.get("kind", 0).asInt());
    string detail = sym.get("detail", "").asString();
    if (!detail.empty())
        o["detail"] = detail;
    Json::Value range = sym.get("range", Json::Value());
    if (range.isObject())
    {
        o["line"] = jInt(range["start"], "line", 0) + 1;
        o["endLine"] = jInt(range["end"], "line", 0) + 1;
    }
    Json::Value children = sym.get("children", Json::Value(Json::arrayValue));
    if (children.isArray() && children.size() > 0)
        o["children"] = symbolsToJson(children);
    return o;
}

Json::Value JZTsLangServer::symbolsToJson(const Json::Value &syms)
{
    Json::Value out(Json::arrayValue);
    if (syms.isArray())
    {
        for (const auto &s : syms)
            out.append(symbolToJson(s));
    }
    return out;
}

static bool rangeContains(const Json::Value &r, const Json::Value &pos)
{
    if (!r.isObject() || !r.isMember("start") || !r.isMember("end"))
        return false;
    int pl = jInt(pos, "line", 0), pc = jInt(pos, "character", 0);
    int sl = jInt(r["start"], "line", 0), sc = jInt(r["start"], "character", 0);
    int el = jInt(r["end"], "line", 0), ec = jInt(r["end"], "character", 0);
    if (pl < sl || pl > el)
        return false;
    if (pl == sl && pc < sc)
        return false;
    if (pl == el && pc > ec)
        return false;
    return true;
}

static int rangeArea(const Json::Value &r)
{
    int sl = jInt(r["start"], "line", 0), sc = jInt(r["start"], "character", 0);
    int el = jInt(r["end"], "line", 0), ec = jInt(r["end"], "character", 0);
    return (el - sl) * 1000000 + (ec - sc);
}

Json::Value JZTsLangServer::symbolAt(const Json::Value &syms, const Json::Value &pos)
{
    Json::Value best;
    int bestArea = INT_MAX;
    for (const auto &sym : syms)
    {
        if (!sym.isObject() || !sym.isMember("range"))
            continue;
        if (!rangeContains(sym["range"], pos))
            continue;
        Json::Value deeper = symbolAt(
            sym.get("children", Json::Value(Json::arrayValue)), pos);
        Json::Value cand = deeper.isNull() ? sym : deeper;
        int area = cand.isMember("range") ? rangeArea(cand["range"]) : INT_MAX;
        if (area < bestArea)
        {
            bestArea = area;
            best = cand;
        }
    }
    return best;
}

Json::Value JZTsLangServer::findSymbolByNameImpl(const Json::Value &syms,
                                                 const string &name, bool preferClass)
{
    // 先序遍历，返回第一个满足条件的同名符号：
    //   preferClass=true  只认 class/interface；
    //   preferClass=false 任意种类。
    // 注意不能"本层暂存 found、深层命中再覆盖"——那会让嵌套的同名成员
    // （如类方法与顶层函数同名）反过来压住先出现的浅层符号。
    for (const auto &sym : syms)
    {
        if (!sym.isObject())
            continue;
        if (sym.get("name", "").asString() == name)
        {
            int kind = sym.get("kind", 0).asInt();
            bool isType = (kind == 5 || kind == 11); // Class / Interface
            if (preferClass ? isType : true)
                return sym;
        }
        Json::Value deeper = findSymbolByNameImpl(
            sym.get("children", Json::Value(Json::arrayValue)), name, preferClass);
        if (!deeper.isNull())
            return deeper;
    }
    return Json::Value();
}

Json::Value JZTsLangServer::findSymbolByName(const Json::Value &syms, const string &name)
{
    Json::Value byType = findSymbolByNameImpl(syms, name, true); // 优先 class/interface
    if (!byType.isNull())
        return byType;
    return findSymbolByNameImpl(syms, name, false); // 退回同名首个匹配
}

void JZTsLangServer::resolveByName(const string &file, const string &name,
                                   function<void(int line, int character)> onPos,
                                   function<void(const string &)> onErr)
{
    string uri = ensureOpen(file);
    Json::Value pp;
    pp["textDocument"]["uri"] = uri;
    callLsp("textDocument/documentSymbol", pp,
            [this, file, name, onPos, onErr](const Json::Value &syms)
            {
                Json::Value sym = findSymbolByName(syms, name);
                if (sym.isNull())
                {
                    onErr("在 " + file + " 中未找到符号 \"" + name + "\"");
                    return;
                }
                Json::Value rp = sym.isMember("selectionRange") ? sym["selectionRange"] : sym["range"];
                int l = rp.isObject() ? jInt(rp["start"], "line", 0) : 0;
                int c = rp.isObject() ? jInt(rp["start"], "character", 0) : 0;
                onPos(l, c);
            },
            onErr);
}

string JZTsLangServer::markupToString(const Json::Value &c)
{
    if (c.isString())
        return c.asString();
    if (c.isArray())
    {
        string s;
        for (const auto &x : c)
        {
            if (!s.empty())
                s += "\n";
            s += markupToString(x);
        }
        return s;
    }
    if (c.isObject())
        return c.get("value", "").asString();
    return string();
}

// 把结果 Json::Value 序列化后回传给工具
static void writeJsonResult(JZTsLangServer::TsCallback cb, const Json::Value &v)
{
    cb(true, Json::FastWriter().write(v));
}

// ==================== 统一查询入口 ====================
void JZTsLangServer::request(TsMethod method, const Json::Value &params, TsCallback cb)
{
    if (!isAvailable())
    {
        cb(false, "TSGO 语言服务不可用：未配置 tsgo(tsc) 二进制路径");
        return;
    }

    ensureStarted();

    if (m_ready && m_proc)
        dispatch(method, params, cb);
    else
        m_queue.push_back({method, params, cb});
}

// ==================== 结构化编辑支持 ====================
void JZTsLangServer::renameSymbol(const string &file, const string &name,
                                  const string &newName, TsCallback cb)
{
    if (!isAvailable())
    {
        cb(false, "TSGO 语言服务不可用：未配置 tsgo(tsc) 二进制路径");
        return;
    }
    ensureStarted();

    auto run = [this, file, newName, cb](int ll, int cc)
    {
        string uri = ensureOpen(file);
        Json::Value p;
        p["textDocument"]["uri"] = uri;
        p["position"]["line"] = ll;
        p["position"]["character"] = cc;
        p["newName"] = newName;
        callLsp("textDocument/rename", p,
                [cb](const Json::Value &r)
                {
                    // 把 WorkspaceEdit（changes / documentChanges 两种形态）归一化为
                    // [{file, edits: [{line, offset, endLine, endOffset, text}]}]，行 1-based。
                    Json::Value out(Json::arrayValue);
                    auto appendOne = [&out](const string &u, const Json::Value &edits)
                    {
                        Json::Value fe;
                        fe["file"] = uriToFile(u);
                        fe["edits"] = Json::Value(Json::arrayValue);
                        for (const auto &e : edits)
                        {
                            Json::Value te;
                            te["line"] = jInt(e["range"]["start"], "line", 0) + 1;
                            te["offset"] = jInt(e["range"]["start"], "character", 0) + 1; // 1-based 列
                            te["endLine"] = jInt(e["range"]["end"], "line", 0) + 1;
                            te["endOffset"] = jInt(e["range"]["end"], "character", 0) + 1;
                            te["text"] = e.get("newText", "").asString();
                            fe["edits"].append(te);
                        }
                        out.append(fe);
                    };
                    Json::Value changes = r.get("changes", Json::Value());
                    if (changes.isObject())
                    {
                        for (const string &u : changes.getMemberNames())
                            appendOne(u, changes[u]);
                    }
                    if (r.isMember("documentChanges") && r["documentChanges"].isArray())
                    {
                        for (const auto &d : r["documentChanges"])
                            appendOne(d["textDocument"].get("uri", "").asString(),
                                      d.get("edits", Json::Value(Json::arrayValue)));
                    }
                    if (out.empty())
                        cb(false, "语言服务未返回可重命名位置");
                    else
                        writeJsonResult(cb, out);
                },
                [cb](const string &e) { cb(false, e); });
    };
    resolveByName(file, name, run,
                  [file, name, cb](const string &e)
                  {
                      cb(false, e + "。可先用 ts_symbols(\"" + name + "\") 搜索其所在文件");
                  });
}

// ==================== 查询分发 ====================
void JZTsLangServer::dispatch(TsMethod method, const Json::Value &params, TsCallback cb)
{
    // ---- 仅依赖 file 的查询 ----
    if (method == TsMethod::Diagnostics || method == TsMethod::Outline)
    {
        string file = params.get("file", "").asString();
        if (file.empty())
        {
            cb(false, "参数 file 不能为空");
            return;
        }
        string uri = ensureOpen(file);
        Json::Value pp;
        pp["textDocument"]["uri"] = uri;
        if (method == TsMethod::Diagnostics)
        {
            callLsp("textDocument/diagnostic", pp,
                    [this, uri, cb](const Json::Value &r)
                    {
                        Json::Value items = r.get("items", Json::Value(Json::arrayValue));
                        Json::Value out(Json::arrayValue);
                        if (items.isArray())
                        {
                            for (const auto &d : items)
                            {
                                Json::Value o;
                                o["file"] = uriToFile(uri);
                                o["line"] = jInt(d["range"]["start"], "line", 0) + 1;
                                o["offset"] = jInt(d["range"]["start"], "character", 0) + 1;
                                o["endLine"] = jInt(d["range"]["end"], "line", 0) + 1;
                                o["endOffset"] = jInt(d["range"]["end"], "character", 0) + 1;
                                o["severity"] = d.get("severity", 0).asInt();
                                o["message"] = d.get("message", "").asString();
                                o["code"] = d.get("code", "").asString();
                                o["source"] = d.get("source", "").asString();
                                out.append(o);
                            }
                        }
                        cb(true, Json::FastWriter().write(out));
                    },
                    [cb](const string &e) { cb(false, e); });
        }
        else // Outline：归一化符号树（1-based 行号、kind 转文本）
        {
            callLsp("textDocument/documentSymbol", pp,
                    [cb](const Json::Value &syms) { writeJsonResult(cb, symbolsToJson(syms)); },
                    [cb](const string &e) { cb(false, e); });
        }
        return;
    }

    // ---- 项目内符号模糊搜索（跨文件定位入口） ----
    if (method == TsMethod::WorkspaceSymbols)
    {
        string query = params.get("query", "").asString();
        if (query.empty())
        {
            cb(false, "参数 query 不能为空");
            return;
        }
        int limit = params.get("limit", 30).asInt();
        if (limit <= 0)
            limit = 30;
        Json::Value pp;
        pp["query"] = query;
        callLsp("workspace/symbol", pp,
                [cb, limit](const Json::Value &r)
                {
                    Json::Value out(Json::arrayValue);
                    if (r.isArray())
                    {
                        int n = 0;
                        for (const auto &s : r)
                        {
                            if (n >= limit)
                                break;
                            Json::Value o;
                            o["name"] = s.get("name", "").asString();
                            o["kind"] = symbolKindName(s.get("kind", 0).asInt());
                            string container = s.get("containerName", "").asString();
                            if (!container.empty())
                                o["container"] = container;
                            Json::Value loc = s.get("location", Json::Value());
                            o["file"] = uriToFile(loc.get("uri", "").asString());
                            Json::Value range = loc.get("range", Json::Value());
                            if (range.isObject())
                            {
                                o["line"] = jInt(range["start"], "line", 0) + 1;
                                o["offset"] = jInt(range["start"], "character", 0) + 1;
                            }
                            out.append(o);
                            n++;
                        }
                    }
                    writeJsonResult(cb, out);
                },
                [cb](const string &e) { cb(false, e); });
        return;
    }

    // ---- 位置类查询：统一 file + name 定位（不再接受行列参数） ----
    string file = params.get("file", "").asString();
    string symName = params.get("name", "").asString();
    if (file.empty())
    {
        cb(false, "参数 file 不能为空");
        return;
    }
    if (symName.empty())
    {
        cb(false, "参数 name（符号名）不能为空。可先用 ts_symbols(query) 在项目内搜索符号"
                  "所在的文件，再用 ts_outline(file) 查看该文件内的符号");
        return;
    }

    resolveByName(file, symName,
                  [this, method, params, file, symName, cb](int ll, int cc)
                  {
                      runPositionQuery(method, params, file, symName, ll, cc, cb);
                  },
                  [file, symName, cb](const string &e)
                  {
                      cb(false, e + "。可先用 ts_symbols(\"" + symName +
                                     "\") 搜索其所在文件，再用 ts_outline 查看该文件符号");
                  });
}

// 位置类查询的实际执行。(line, character) 是符号名经 documentSymbol 反查出的
// LSP 0-based 坐标，此后各查询与编辑器光标行为完全一致。
void JZTsLangServer::runPositionQuery(TsMethod method, const Json::Value &params,
                                      const string &file, const string &symName,
                                      int line, int character, TsCallback cb)
{
    (void)file;
    string uri = ensureOpen(file);
    Json::Value pp;
    pp["textDocument"]["uri"] = uri;
    pp["position"]["line"] = line;
    pp["position"]["character"] = character;

    switch (method)
    {
        case TsMethod::Definition:
            callLsp("textDocument/definition", pp,
                    [cb](const Json::Value &r) { writeJsonResult(cb, normalizeLocations(r)); },
                    [cb](const string &e) { cb(false, e); });
            break;

        case TsMethod::References:
        {
            Json::Value p;
            p["textDocument"]["uri"] = uri;
            p["position"]["line"] = line;
            p["position"]["character"] = character;
            p["context"]["includeDeclaration"] = true; // 定义本身也计入，方便模型判断
            callLsp("textDocument/references", p,
                    [this, cb](const Json::Value &r)
                    {
                        Json::Value locs = normalizeLocations(r);
                        // 每条引用补上所在行源码片段，模型无需再按行号翻文件。
                        // 按文件缓存行数组，避免逐条重复读盘。
                        map<string, vector<string>> lineCache;
                        for (Json::Value::ArrayIndex i = 0; i < locs.size(); i++)
                        {
                            string f = locs[i].get("file", "").asString();
                            int ln = locs[i].get("line", 0).asInt();
                            auto it = lineCache.find(f);
                            if (it == lineCache.end())
                                it = lineCache.emplace(f, splitLines(readFileUtf8(f))).first;
                            const vector<string> &lines = it->second;
                            string snippet;
                            if (ln >= 1 && ln <= (int)lines.size())
                            {
                                snippet = str_trimmed(lines[ln - 1]);
                                if ((int)snippet.size() > 300)
                                    snippet = snippet.substr(0, 300) + "...";
                            }
                            locs[i]["text"] = snippet;
                        }
                        writeJsonResult(cb, locs);
                    },
                    [cb](const string &e) { cb(false, e); });
            break;
        }

        case TsMethod::QuickInfo:
            callLsp("textDocument/hover", pp,
                    [cb, symName](const Json::Value &r)
                    {
                        Json::Value out;
                        out["name"] = symName;
                        out["contents"] = markupToString(r.get("contents", Json::Value()));
                        writeJsonResult(cb, out);
                    },
                    [cb](const string &e) { cb(false, e); });
            break;

        case TsMethod::Source:
        {
            int maxc = params.isMember("max_chars") ? params["max_chars"].asInt() : 8000;
            // 1) definition 定位声明位置（import/export 别名自动解析到真正的定义文件）
            callLsp("textDocument/definition", pp,
                    [this, maxc, cb](const Json::Value &def)
                    {
                        Json::Value loc = firstLocation(def);
                        if (loc.isNull())
                        {
                            cb(false, "未找到符号定义");
                            return;
                        }
                        string defUri = loc.isMember("targetUri") ? loc["targetUri"].asString()
                                                                 : loc.get("uri", "").asString();
                        Json::Value range = loc.isMember("targetRange")
                                                ? loc["targetRange"]
                                                : loc.get("range", Json::Value());
                        // 2) documentSymbol 取该文件符号树，找包含声明位置的完整符号范围
                        Json::Value sp;
                        sp["textDocument"]["uri"] = defUri;
                        callLsp("textDocument/documentSymbol", sp,
                                [this, defUri, range, maxc, cb](const Json::Value &syms)
                                {
                                    Json::Value pos = range.isObject() ? range["start"] : Json::Value();
                                    Json::Value sym = symbolAt(syms, pos);
                                    Json::Value useRange = sym.isNull() ? range : sym.get("range", range);
                                    string defFile = uriToFile(defUri);
                                    Json::Value text = readFileRange(defFile, useRange);
                                    string s = text.isString() ? text.asString() : string();
                                    if ((int)s.size() > maxc)
                                        s = s.substr(0, maxc) + "\n...(源码过长已截断)";
                                    Json::Value out;
                                    out["file"] = defFile;
                                    out["line"] = jInt(useRange["start"], "line", 0) + 1;
                                    out["endLine"] = jInt(useRange["end"], "line", 0) + 1;
                                    out["source"] = s;
                                    writeJsonResult(cb, out);
                                },
                                [cb](const string &e) { cb(false, e); });
                    },
                    [cb](const string &e) { cb(false, e); });
            break;
        }

        case TsMethod::Members:
        {
            int maxm = params.isMember("max_members") ? params["max_members"].asInt() : 100;
            Json::Value pos;
            pos["line"] = line;
            pos["character"] = character;
            Json::Value sp;
            sp["textDocument"]["uri"] = uri;
            callLsp("textDocument/documentSymbol", sp,
                    [this, pos, maxm, cb](const Json::Value &syms)
                    {
                        Json::Value sym = symbolAt(syms, pos);
                        Json::Value members = sym.isNull() ? syms
                                                          : sym.get("children", Json::Value(Json::arrayValue));
                        Json::Value all = symbolsToJson(members);
                        if (maxm > 0 && all.isArray() && (int)all.size() > maxm)
                        {
                            Json::Value trimmed(Json::arrayValue);
                            for (Json::Value::ArrayIndex i = 0; i < (Json::Value::ArrayIndex)maxm; i++)
                                trimmed.append(all[i]);
                            all = trimmed;
                        }
                        writeJsonResult(cb, all);
                    },
                    [cb](const string &e) { cb(false, e); });
            break;
        }

        default:
            cb(false, "不支持的查询类型");
            break;
    }
}
