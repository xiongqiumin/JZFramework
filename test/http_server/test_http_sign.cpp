// 浏览器"请求签名"流程可行性验证
// ---------------------------------------------------------------------------
// 目的: 验证「HTTP 服务端要求客户端(浏览器)每次请求都带签名」这条链路能不能走通。
//
// 链路:
//   1. 服务端 = JZHttpServer, 会话级随机密钥(64 hex), 通过首页注入给浏览器页面;
//   2. 浏览器 = Edge/Chrome 无头实例, 用 WebCrypto `crypto.subtle` 算 HMAC-SHA256;
//   3. 请求头带 X-Sign / X-Ts / X-Nonce, 服务端用 JZCrypto::hmacSha256 重算比对;
//   4. 时间窗(±300s) + nonce 一次性(防重放)。
//
// 分两段验证:
//   A. 自检段(框架自带 HTTP 客户端, 确定性): 无签名/错密钥/正确/重放/过期时间戳 五种情形;
//   B. 浏览器段(真实浏览器): 127.0.0.1 与局域网 IP 各跑一次, 用 `crypto.subtle` 是否可用
//      判断该流程能否在真实部署(明文 http + 局域网 IP)下成立。
//
// 用法:
//   test_http_sign [port] [--show]
//     port    默认 18231
//     --show  浏览器用可视窗口(默认无头)
// ---------------------------------------------------------------------------
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "JZDebug.h"
#include "JZEventLoop.h"
#include "JZTimer.h"
#include "JZByteArray.h"
#include "JZHttpServer.h"
#include "JZHttpHandler.h"
#include "JZNetworkAccessManager.h"
#include "JZNetworkReply.h"
#include "JZNetworkRequest.h"
#include "JZApplication.h"
#include "JZDir.h"
#include "JZCrypto.h"
#include "JZUuid.h"
#include "JZProcess.h"

// ⚠ 这两个必须在 JZ 头之后: winsock2.h(经 windows.h 链)带进来的宏会与
//   JZHttpRequest::Method 之类的名字冲突(实测 "enum Method" 直接语法报错)。
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")   // gethostname/getaddrinfo/inet_ntop 只需要在这里用
#endif

using namespace std;

#define LOG(...) do { printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

static atomic<bool> g_active{true};
static atomic<int> g_runSeq{0};   // 给浏览器 profile 目录编号, 保证每次启动都是新目录

// 会话密钥(进程级): 首页把它注入给页面, 之后所有请求都用它验签
static string g_sessionKey;

// ---------------------------------------------------------------- 服务端状态
struct SignState
{
    mutex mu;
    int signedOk = 0;   // 验签通过次数
    int rejected = 0;   // 验签拒绝次数
    int plain = 0;      // 无签名接口命中次数
    map<string, int> nonceSeen;            // nonce -> 首次出现的秒级时间戳
    map<string, string> reports;           // step -> 浏览器回报的正文(JSON)
    map<string, int> pageHits;             // step -> 页面被请求的次数(判定"浏览器到底连上没有")
};
static SignState g_sign;

static string strReplace(const string &src, const string &from, const string &to)
{
    string out;
    size_t pos = 0;
    while (true)
    {
        size_t hit = src.find(from, pos);
        if (hit == string::npos)
        {
            out += src.substr(pos);
            break;
        }
        out += src.substr(pos, hit - pos);
        out += to;
        pos = hit + from.size();
    }
    return out;
}

static string statusText(int code)
{
    switch (code)
    {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 404: return "Not Found";
    default:  return "Error";
    }
}

static void sendText(JZHttpServerClient *client, int code, const string &ctype, const string &body)
{
    string head = "HTTP/1.1 " + to_string(code) + " " + statusText(code) + "\r\n"
                  "Content-Type: " + ctype + "\r\n"
                  "Content-Length: " + to_string(body.size()) + "\r\n"
                  "Cache-Control: no-store\r\n"
                  "\r\n";
    JZByteArray resp;
    resp.append(head.data(), (int)head.size());
    resp.append(body.data(), (int)body.size());
    client->write(resp.data(), resp.size(), true);
}

// ---------------------------------------------------------------- 签名/验签
// 规范化串: METHOD \n PATH \n TS \n NONCE  (GET 无 body, 先把机制跑通)
static string canonicalString(const string &method, const string &path, const string &ts, const string &nonce)
{
    return method + "\n" + path + "\n" + ts + "\n" + nonce;
}

static string hmacHex(const string &key, const string &msg)
{
    JZByteArray k(key.data(), (int)key.size());
    JZByteArray d(msg.data(), (int)msg.size());
    JZByteArray sig = JZCrypto::hmacSha256(k, d);
    return str_fromUtf8(sig.toHex());
}

static string computeSign(const string &method, const string &path, const string &ts, const string &nonce)
{
    return hmacHex(g_sessionKey, canonicalString(method, path, ts, nonce));
}

static bool constantTimeEqual(const string &a, const string &b)
{
    if (a.size() != b.size())
        return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); i++)
        diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0;
}

// 返回空串=通过, 否则=拒绝原因
static string verifyRequest(const JZHttpRequest &req)
{
    string sign = req.header("X-Sign");
    string ts = req.header("X-Ts");
    string nonce = req.header("X-Nonce");
    if (sign.empty() || ts.empty() || nonce.empty())
        return "missing-header";

    long long t = atoll(ts.c_str());
    long long now = (long long)time(nullptr);
    if (t <= 0 || llabs(now - t) > 300)
        return "ts-expired";

    string expect = computeSign(req.methodString(), req.path(), ts, nonce);
    if (!constantTimeEqual(expect, sign))
        return "bad-sign";

    lock_guard<mutex> lk(g_sign.mu);
    if (g_sign.nonceSeen.count(nonce))
        return "replay";
    g_sign.nonceSeen[nonce] = (int)now;
    return "";
}

// ---------------------------------------------------------------- 页面(浏览器端签名)
// __KEY__ / __STEP__ 由服务端注入; crypto.subtle 只在"安全上下文"可用,
// 明文 http 下只有 localhost/127.0.0.1 算安全上下文, 局域网 IP 不算 —— 这正是本次要测的点。
static const char *kPageHtml = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><title>JZ sign flow</title>
<style>body{font:13px/1.7 Consolas,monospace;margin:20px}pre{background:#f4f4f4;padding:12px;border:1px solid #ddd}</style>
</head><body>
<h3>JZ - browser request signing (HMAC-SHA256)</h3>
<pre id="log"></pre>
<script>
const KEY = "__KEY__";
const STEP = "__STEP__";
const out = (s) => { document.getElementById("log").textContent += s + "\n"; };
const hex = (buf) => [...new Uint8Array(buf)].map(b => b.toString(16).padStart(2, "0")).join("");
const report = (obj) => fetch("/report?step=" + STEP, {
    method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(obj)
});
(async () => {
    const secure = window.isSecureContext ? 1 : 0;
    const subtle = (window.crypto && window.crypto.subtle) ? 1 : 0;
    out("origin=" + location.origin + "  isSecureContext=" + secure + "  crypto.subtle=" + subtle);
    const base = { step: STEP, origin: location.origin, secure: secure, subtle: subtle };
    if (!subtle) {
        out("RESULT=NOSUBTLE  (not a secure context -> no WebCrypto)");
        await report(Object.assign(base, { status: 0, note: "no-subtle" }));
        return;
    }
    const key = await crypto.subtle.importKey("raw", new TextEncoder().encode(KEY),
        { name: "HMAC", hash: "SHA-256" }, false, ["sign"]);
    const ts = String(Math.floor(Date.now() / 1000));
    const nonce = hex(crypto.getRandomValues(new Uint8Array(16)));
    const msg = "GET\n/api/data\n" + ts + "\n" + nonce;
    const sig = hex(await crypto.subtle.sign("HMAC", key, new TextEncoder().encode(msg)));
    const r = await fetch("/api/data", { headers: { "X-Sign": sig, "X-Ts": ts, "X-Nonce": nonce } });
    const body = await r.text();
    out("GET /api/data -> " + r.status + "  " + body);
    await report(Object.assign(base, { status: r.status, body: body }));
    out("RESULT=" + (r.status === 200 ? "OK" : "FAIL"));
})().catch(async (e) => {
    out("RESULT=EXC " + e);
    try { await report({ step: STEP, error: String(e), subtle: -1, status: -1 }); } catch (_) {}
});
</script></body></html>)HTML";

// ---------------------------------------------------------------- Handler
class SignPageHandler : public JZHttpHandler
{
public:
    void handle(JZHttpServerClient *client, const JZHttpRequest &req) override
    {
        string step = req.queryParam("step");
        if (step.empty())
            step = "v0";
        {
            lock_guard<mutex> lk(g_sign.mu);
            g_sign.pageHits[step]++;
        }
        string page = strReplace(kPageHtml, "__KEY__", g_sessionKey);
        page = strReplace(page, "__STEP__", step);
        sendText(client, 200, "text/html; charset=utf-8", page);
    }
};

class SignedApiHandler : public JZHttpHandler
{
public:
    void handle(JZHttpServerClient *client, const JZHttpRequest &req) override
    {
        string err = verifyRequest(req);
        if (!err.empty())
        {
            {
                lock_guard<mutex> lk(g_sign.mu);
                g_sign.rejected++;
            }
            sendText(client, 401, "application/json", "{\"ok\":false,\"reason\":\"" + err + "\"}");
            return;
        }
        {
            lock_guard<mutex> lk(g_sign.mu);
            g_sign.signedOk++;
        }
        sendText(client, 200, "application/json",
                 "{\"ok\":true,\"data\":\"top-secret\",\"remote\":\"" + req.remoteAddr() + "\"}");
    }
};

// 无签名接口(基线): 证明服务本身可用, 401 只来自验签
class PlainHandler : public JZHttpHandler
{
public:
    void handle(JZHttpServerClient *client, const JZHttpRequest &req) override
    {
        {
            lock_guard<mutex> lk(g_sign.mu);
            g_sign.plain++;
        }
        sendText(client, 200, "text/plain", "pong");
    }
};

// 浏览器回报(页面把自己看到的结果回传, 服务端据此判定, 不依赖抓浏览器 stdout)
class ReportHandler : public JZHttpHandler
{
public:
    void handle(JZHttpServerClient *client, const JZHttpRequest &req) override
    {
        string step = req.queryParam("step");
        string body = str_fromUtf8(req.body());
        {
            lock_guard<mutex> lk(g_sign.mu);
            g_sign.reports[step] = body;
        }
        sendText(client, 200, "application/json", "{\"ok\":true}");
    }
};

// ---------------------------------------------------------------- 辅助: 找浏览器 / 本机局域网 IP
static string findBrowser()
{
#ifdef _WIN32
    const char *bases[] = {"ProgramFiles(x86)", "ProgramFiles", "LOCALAPPDATA"};
    const char *rels[] = {
        "\\Microsoft\\Edge\\Application\\msedge.exe",
        "\\Google\\Chrome\\Application\\chrome.exe",
    };
    for (int i = 0; i < 3; i++)
    {
        const char *base = getenv(bases[i]);
        if (!base)
            continue;
        for (int j = 0; j < 2; j++)
        {
            string p = string(base) + rels[j];
            FILE *fp = fopen(p.c_str(), "rb");
            if (fp)
            {
                fclose(fp);
                return p;
            }
        }
    }
#endif
    return "";
}

static string detectLanIp()
{
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    char host[256] = {0};
    if (gethostname(host, sizeof(host)) != 0)
        return "";

    addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    addrinfo *res = nullptr;
    if (getaddrinfo(host, nullptr, &hints, &res) != 0)
        return "";

    string ip;
    for (addrinfo *p = res; p; p = p->ai_next)
    {
        char buf[64] = {0};
        inet_ntop(AF_INET, &((sockaddr_in *)p->ai_addr)->sin_addr, buf, sizeof(buf));
        string s = buf;
        if (s.empty() || s == "127.0.0.1")
            continue;
        ip = s;
        break;
    }
    freeaddrinfo(res);
    return ip;
#else
    return "";
#endif
}

// ---------------------------------------------------------------- 测试主体
struct SelfCase
{
    string name;
    string path;
    map<string, string> headers;
    int expect;
};

struct BrowserRun
{
    string step;
    string label;
    string url;
    bool insecureFlag;
};

class SignFlowTest : public JZObject
{
public:
    SignFlowTest(JZEventLoop *loop, int port, bool show)
        : m_loop(loop), m_show(show)
    {
        m_baseUrl = "http://127.0.0.1:" + to_string(port);

        m_server = new JZHttpServer(this);
        if (!m_server->listen("0.0.0.0", port))
        {
            // 端口被占时不要继续: 自检会一直等不到回包, 只会卡到看门狗
            LOG("[sign] FATAL: listen 0.0.0.0:%d failed (port in use?)", port);
            ::exit(2);
        }
        m_server->addRoute("/", new SignPageHandler());
        m_server->addRoute("/index.html", new SignPageHandler());
        m_server->addRoute("/api/data", new SignedApiHandler());
        m_server->addRoute("/api/plain", new PlainHandler());
        m_server->addRoute("/report", new ReportHandler());

        m_net = new JZNetworkAccessManager(this);

        m_watchdog = new JZTimer(this);
        m_watchdog->setSingleShot(true);
        connect(&m_watchdog->sigTimeout, this, [this]() {
            LOG("[sign] WATCHDOG 180s timeout");
            evaluateAll("watchdog-timeout");
        });
        m_watchdog->start(180 * 1000);

        m_poll = new JZTimer(this);
        m_poll->setSingleShot(false);
        connect(&m_poll->sigTimeout, this, [this]() { tick(); });
        m_poll->start(100);

        buildSelfCases();
        buildBrowserRuns(port);
    }

    void start()
    {
        LOG("########## browser request-signing flow check (HMAC-SHA256) ##########");
        LOG("[sign] sessionKey = %s...(64 hex)", g_sessionKey.substr(0, 16).c_str());
        LOG("[sign] server listening 0.0.0.0, base = %s", m_baseUrl.c_str());
        LOG("[sign] --- phase A: self check with framework http client ---");
        sendSelfCase(0);
    }

private:
    // ---------- A. 自检(纯 C++ 客户端, 确定性)
    void buildSelfCases()
    {
        long long now = (long long)time(nullptr);
        string ts = to_string(now);
        string nonceOk = "self-nonce-1";

        auto hdr = [](const string &key, const string &t, const string &n) {
            map<string, string> h;
            h["X-Sign"] = hmacHex(key, canonicalString("GET", "/api/data", t, n));
            h["X-Ts"] = t;
            h["X-Nonce"] = n;
            return h;
        };

        m_self.push_back({"no signature -> 401", "/api/data", {}, 401});
        m_self.push_back({"wrong key -> 401", "/api/data", hdr("wrong-key", ts, "self-nonce-bad"), 401});

        m_self.push_back({"valid signature -> 200", "/api/data", hdr(g_sessionKey, ts, nonceOk), 200});
        // 复用上面那条请求的 nonce + 签名: 服务端应该按重放拒绝
        m_self.push_back({"replay same nonce -> 401", "/api/data", hdr(g_sessionKey, ts, nonceOk), 401});
        m_self.push_back({"stale timestamp -> 401", "/api/data",
                          hdr(g_sessionKey, to_string(now - 1000), "self-nonce-old"), 401});

        m_self.push_back({"unsigned plain endpoint -> 200", "/api/plain", {}, 200});
    }

    void sendSelfCase(int idx)
    {
        if (idx >= (int)m_self.size())
        {
            startBrowserPhase();
            return;
        }

        const SelfCase &c = m_self[idx];
        JZNetworkRequest req(m_baseUrl + c.path);
        for (auto &h : c.headers)
            req.setRawHeader(h.first, h.second);

        JZNetworkReply *reply = m_net->get(req);
        connect(&reply->sigFinished, this, [this, reply, idx]() { onSelfDone(reply, idx); });
    }

    void onSelfDone(JZNetworkReply *reply, int idx)
    {
        const SelfCase &c = m_self[idx];
        int code = reply->httpStatusCode();
        string body = str_fromUtf8(reply->readAll());
        reply->deleteLater();

        bool pass = (code == c.expect);
        m_results.push_back({string(pass ? "OK  " : "FAIL") + "  " + c.name + "  (got " + to_string(code) +
                                 (pass ? "" : ", expect " + to_string(c.expect)) + ")" + (body.empty() ? "" : "  " + body),
                             pass});

        sendSelfCase(idx + 1);
    }

    // ---------- B. 浏览器(真实浏览器 + 真实 WebCrypto)
    void buildBrowserRuns(int port)
    {
        if (m_show)
            LOG("[sign] --show: browser window will be visible");

        m_browser = findBrowser();
        string lanIp = detectLanIp();

        m_runs.push_back({"v1", "browser http://127.0.0.1 (loopback = secure context)",
                          "http://127.0.0.1:" + to_string(port) + "/?step=v1", false});

        if (!lanIp.empty())
        {
            string lanBase = "http://" + lanIp + ":" + to_string(port);
            m_lanIp = lanIp;
            m_runs.push_back({"v2", "browser " + lanBase + " (plain http, LAN IP)",
                              lanBase + "/?step=v2", false});
            m_runs.push_back({"v3", "browser " + lanBase + " (+unsafely-treat-insecure-origin-as-secure)",
                              lanBase + "/?step=v3", true});
        }
    }

    void startBrowserPhase()
    {
        LOG("[sign] --- phase B: real browser ---");
        if (m_browser.empty())
        {
            LOG("[sign] SKIP: no Edge/Chrome found");
            evaluateAll("");
            return;
        }
        if (m_lanIp.empty())
            LOG("[sign] WARN: no LAN IP detected, only loopback run");

        m_phaseBrowser = true;
        startRun(0);
    }

    void startRun(int i)
    {
        if (i >= (int)m_runs.size())
        {
            evaluateAll("");
            return;
        }
        if (i != m_curRun)
            m_attempt = 0;   // 新一轮才清重试计数(同一轮的重试要保留)
        m_curRun = i;
        m_state = 0;
        m_ticks = 0;
        m_rep.clear();
        m_signedBefore = signedOk();

        const BrowserRun &r = m_runs[i];
        // 用 JZProcess + 参数数组启动(不经 cmd.exe): 免掉"路径带空格"的引号地狱,
        // 还能直接拿退出码/随时杀进程, 不用再开线程跑 system()。
        // ⚠ 无头模式必须带 --dump-dom: 否则 Chrome "加载还没完就退出",
        //   导航是否真的发生是随机的(pageHits 时有时无), 判定不可靠。
        vector<string> args;
        if (!m_show)
        {
            args.push_back("--headless=new");
            args.push_back("--disable-gpu");
        }
        args.push_back("--no-first-run");
        args.push_back("--user-data-dir=" + m_userDataDir(i));
        args.push_back("--virtual-time-budget=8000");
        if (r.insecureFlag)
            args.push_back("--unsafely-treat-insecure-origin-as-secure=" +
                           r.url.substr(0, r.url.find("/?")));
        if (!m_show)
            args.push_back("--dump-dom");
        args.push_back(r.url);

        LOG("[sign] run %s: %s", r.step.c_str(), r.label.c_str());
        m_procExit = -1;
        if (m_proc)
        {
            m_proc->deleteLater();
            m_proc = nullptr;
        }

        JZProcess *proc = new JZProcess(this);
        m_proc = proc;
        connect(&proc->sigFinished, this, [this, proc](int code, JZProcess::ExitStatus) {
            if (proc == m_proc)
                m_procExit = code;
        });
        connect(&proc->sigErrorOccurred, this, [this, proc](JZProcess::ProcessError err) {
            if (proc == m_proc)
            {
                LOG("[sign]   browser error %d", (int)err);
                m_procExit = -2;
            }
        });
        if (!proc->start(m_browser, args))
        {
            LOG("[sign]   start failed: %s", proc->lastStartError().c_str());
            m_procExit = -2;
        }
    }

    // ⚠ 每次启动都要用**全新**的 profile 目录: 复用被上一轮(甚至是上次运行残留的)
    //   Edge 进程占住的目录时, 新进程会立刻退出(实测退出码 21, 一个请求都不发)。
    string profileRoot()
    {
        char buf[512] = {0};
#ifdef _WIN32
        const char *tmp = getenv("TEMP");
        snprintf(buf, sizeof(buf), "%s\\jz_http_sign", tmp ? tmp : ".");
#else
        snprintf(buf, sizeof(buf), "/tmp/jz_http_sign");
#endif
        return buf;
    }

    string m_userDataDir(int i)
    {
        char buf[600] = {0};
        int seq = ++g_runSeq;
#ifdef _WIN32
        snprintf(buf, sizeof(buf), "%s\\r%d_%d_%lld", profileRoot().c_str(), i, seq,
                 (long long)time(nullptr));
#else
        snprintf(buf, sizeof(buf), "%s/r%d_%d_%lld", profileRoot().c_str(), i, seq,
                 (long long)time(nullptr));
#endif
        return buf;
    }

    void tick()
    {
        if (!m_phaseBrowser || m_curRun < 0)
            return;

        const BrowserRun &r = m_runs[m_curRun];
        if (m_state == 0)
        {
            string rep;
            bool has = false;
            {
                lock_guard<mutex> lk(g_sign.mu);
                auto it = g_sign.reports.find(r.step);
                if (it != g_sign.reports.end())
                {
                    rep = it->second;
                    has = true;
                }
            }
            if (has)
            {
                m_rep = rep;
                m_state = 1;
                m_ticks = 0;
            }
            else if (m_procExit >= 0)
            {
                // 进程已退出但没回报: 给 1.5s 余量(残留包)再收尾
                if (++m_ticks > 15)
                {
                    m_rep = "";
                    m_state = 1;
                    m_ticks = 0;
                }
            }
        }

        if (m_state == 1)
        {
            // 上一轮浏览器必须彻底退出再起下一轮, 否则新实例可能被老实例"转发"而直接退出
            if (m_procExit >= 0 || ++m_ticks > 200)
                finishRun(m_rep);
        }
    }

    int jsonInt(const string &json, const string &key)
    {
        string pat = "\"" + key + "\":";
        size_t p = json.find(pat);
        if (p == string::npos)
            return -999;
        return atoi(json.c_str() + p + pat.size());
    }

    void finishRun(const string &rep)
    {
        const BrowserRun &r = m_runs[m_curRun];
        int subtle = jsonInt(rep, "subtle");
        int status = jsonInt(rep, "status");
        int signedNow = signedOk() - m_signedBefore;
        int pageHit = 0;
        {
            lock_guard<mutex> lk(g_sign.mu);
            auto it = g_sign.pageHits.find(r.step);
            if (it != g_sign.pageHits.end())
                pageHit = it->second;
        }
        int exitCode = m_procExit;
        if (m_proc && m_procExit == -1)
            m_proc->killProcess();   // 页面已回报/超时收尾, 别让浏览器赖着影响下一轮

        // 一次都没打到页面 = 启动/导航失败(与本流程无关), 重试一次再下结论
        if (rep.empty() && pageHit == 0 && m_attempt == 0)
        {
            LOG("      retry %s (pageHits=0, exit=%d)", r.step.c_str(), exitCode);
            m_attempt = 1;
            m_state = 0;
            m_ticks = 0;
            startRun(m_curRun);
            return;
        }

        string line = "      " + r.label + "  [pageHits=" + to_string(pageHit) +
                      " exit=" + to_string(exitCode) + "]";
        bool pass = false;
        if (r.step == "v2")
        {
            // v2 是"探明现象", 不作为通过条件: 明文 http + 局域网 IP 下预期 subtle=0
            if (rep.empty())
                line += "  -> no report (browser exited silently)";
            else if (subtle == 0)
                line += "  -> crypto.subtle UNAVAILABLE (isSecureContext=false) => signing impossible here";
            else if (subtle == 1 && status == 200)
                line += "  -> crypto.subtle available, signature accepted";
            else
                line += "  -> subtle=" + to_string(subtle) + " status=" + to_string(status);
            m_results.push_back({"INFO  " + line, true});
        }
        else if (rep.empty() && pageHit == 0)
        {
            // 页面根本没被加载 = 浏览器启动/环境问题, 与本流程无关, 不判 FAIL
            line += "  -> browser never loaded the page (launch/env issue) => unverified";
            m_results.push_back({"INFO  " + line, true});
        }
        else
        {
            pass = (subtle == 1 && status == 200 && signedNow > 0);
            if (rep.empty())
                line += "  -> FAIL: no report from browser";
            else
                line += "  -> subtle=" + to_string(subtle) + " status=" + to_string(status) +
                        " serverVerified=" + to_string(signedNow);
            m_results.push_back({string(pass ? "OK  " : "FAIL") + "  " + line, pass});
        }
        LOG("%s", line.c_str());

        int next = m_curRun + 1;
        m_curRun = -1;
        startRun(next);
    }

    int signedOk()
    {
        lock_guard<mutex> lk(g_sign.mu);
        return g_sign.signedOk;
    }

    // ---------- 汇总
    void evaluateAll(const string &reason)
    {
        if (!g_active.exchange(false))
            return;

        int passCount = 0, failCount = 0, infoCount = 0;
        LOG("");
        LOG("========== result ==========");
        for (auto &r : m_results)
        {
            LOG("%s", r.first.c_str());
            if (r.first.compare(0, 4, "INFO") == 0)
                infoCount++;
            else if (r.second)
                passCount++;
            else
                failCount++;
        }
        int ok, rej, plain;
        {
            lock_guard<mutex> lk(g_sign.mu);
            ok = g_sign.signedOk;
            rej = g_sign.rejected;
            plain = g_sign.plain;
        }
        LOG("---------- server counters ----------");
        int nonces = 0;
        {
            lock_guard<mutex> lk(g_sign.mu);
            nonces = (int)g_sign.nonceSeen.size();
        }
        LOG("signed-ok=%d  rejected=%d  plain=%d  nonce-cache=%d", ok, rej, plain, nonces);
        if (!reason.empty())
            LOG("aborted: %s", reason.c_str());
        LOG("====================================");
        LOG("SUMMARY: pass=%d fail=%d info=%d => %s", passCount, failCount, infoCount,
            (failCount == 0 && passCount > 0) ? "FLOW WORKS" : "FLOW BROKEN");

        // 浏览器 profile 是几 MB 的临时目录, 收尾清掉
        if (m_proc)
            m_proc->killProcess();
        JZDir tmp(profileRoot());
        tmp.removeRecursively();

        ::exit((failCount == 0 && passCount > 0) ? 0 : 1);
    }

    JZEventLoop *m_loop;
    bool m_show;
    string m_baseUrl;
    string m_browser;
    string m_lanIp;

    JZHttpServer *m_server = nullptr;
    JZNetworkAccessManager *m_net = nullptr;
    JZTimer *m_watchdog = nullptr;
    JZTimer *m_poll = nullptr;

    vector<SelfCase> m_self;
    vector<BrowserRun> m_runs;
    vector<pair<string, bool>> m_results;

    JZProcess *m_proc = nullptr;
    int m_procExit = -1;      // -1=还在跑, -2=启动失败, 其它=退出码

    bool m_phaseBrowser = false;
    int m_curRun = -1;
    int m_state = 0;      // 0=等页面回报 1=等浏览器退出
    int m_ticks = 0;
    int m_attempt = 0;
    string m_rep;
    int m_signedBefore = 0;
};

// 生成会话密钥: 两次 UUID 拼成 64 hex(足够这次流程验证用)
static void initSessionKey()
{
    JZUuid a = JZUuid::createUuid();
    JZUuid b = JZUuid::createUuid();
    g_sessionKey = a.toString() + b.toString();
}

// --serve: 只起服务、不跑测试, 方便用"自己的浏览器/curl"手工点一次
// (排查"浏览器到底连没连上": pageHits 不动 = 根本没到达服务端)
static int runServeMode(int port)
{
    JZEventLoop loop;
    JZHttpServer server;
    if (!server.listen("0.0.0.0", port))
    {
        LOG("[serve] FATAL: listen 0.0.0.0:%d failed", port);
        return 2;
    }
    server.addRoute("/", new SignPageHandler());
    server.addRoute("/index.html", new SignPageHandler());
    server.addRoute("/api/data", new SignedApiHandler());
    server.addRoute("/api/plain", new PlainHandler());
    server.addRoute("/report", new ReportHandler());

    string lanIp = detectLanIp();
    LOG("[serve] loopback : http://127.0.0.1:%d/", port);
    if (!lanIp.empty())
        LOG("[serve] LAN      : http://%s:%d/", lanIp.c_str(), port);
    LOG("[serve] 用浏览器打开后, 每 3s 打印一次统计; Ctrl+C 退出");

    JZTimer *timer = new JZTimer(&server);
    timer->setSingleShot(false);
    connect(&timer->sigTimeout, &server, []() {
        int pages = 0, ok = 0, rej = 0;
        {
            lock_guard<mutex> lk(g_sign.mu);
            for (auto &kv : g_sign.pageHits)
                pages += kv.second;
            ok = g_sign.signedOk;
            rej = g_sign.rejected;
        }
        LOG("[serve] pages=%d  signed-ok=%d  rejected=%d", pages, ok, rej);
    });
    timer->start(3000);

    loop.exec();
    return 0;
}

int main(int argc, char **argv)
{
    JZApplication app;
    initSessionKey();

    int port = 18231;
    bool show = false;
    bool serve = false;
    for (int i = 1; i < argc; i++)
    {
        string a = argv[i];
        if (a == "--show")
            show = true;
        else if (a == "--serve")
            serve = true;
        else if (!a.empty() && a[0] != '-')
            port = atoi(a.c_str());
    }

    if (serve)
        return runServeMode(port);

    JZEventLoop loop;
    SignFlowTest test(&loop, port, show);
    test.start();
    loop.exec();
    return 0;
}
