#include "JZAgentCliServer.h"

#include "JZNet/JZNetServer.h"   // 控制面(hub): cli 出站连进来的 JZNetServer
#include "JZAgentHubPacket.h"    // 控制面协议(cli <-> server)
#include "JZHttpServer.h"        // 后台网页 / 管理 API
#include "JZHttpStaticFiles.h"
#include "JZInprocCli.h"          // inproc 模式: cli 跑在同进程的独立线程里(源码引用)
#include "JZCliConfigIO.h"        // jzCliContextWindow(内置 agent 的上下文窗口)
#include "JZDir.h"               // mkpath(data 目录)
#include "JZPlatform.h"          // applicationDirPath
#include "JZFileInfo.h"          // exists
#include "JZMutex.h"             // JZMutex / JZMutexLocker(日志跨线程读写保护)
#include "JZDateTime.h"          // currentMSecsSinceEpoch
#include "JZTimer.h"             // 任务超时(到点回 504 并请求 cli 取消)
#include "JZDebug.h"
#include "JZLog.h"            // jzLogDirOf: /api/agents/logs 读 Agent.log 尾部
#include "JZJson.h"              // Json::Value / FastWriter
#include "JZCodeTools.h"         // codeNormalizePath(切换工作目录时规范化路径)
#include "JZFileOps.h"           // 文件树/目录选择器: 本机执行(远端走控制面转发到 cli)
#include "JZStd.h"               // str_trimmed(CSRF: Sec-Fetch-Site / Origin / Referer 判来源)

#include <algorithm>
#include <cstdio>        // snprintf(SSE chunked 长度前缀)
#include <ctime>         // gmtime / strftime(TaskStatus.timestamp)
#include <deque>
#include <fstream>       // 管理台页面从磁盘读(server_web/index.html)
#include <functional>    // std::hash: contextId 被改写成安全文件名时补个防撞后缀
#include <sstream>

using namespace std;

// 慢请求探针: 本地部署没有"网络抖动", 页面上的"请求超时"只可能是这里阻塞了事件循环。
// 超过 500ms 就往 stderr 打一行(启动脚本重定向到 logs/server.err.txt), 直接看是哪条接口。
struct SlowReqGuard
{
    const char *name;
    long long t0;
    explicit SlowReqGuard(const char *n) : name(n), t0(JZPlatform::getTickCountMs()) {}
    ~SlowReqGuard()
    {
        const long long dt = JZPlatform::getTickCountMs() - t0;
        if (dt >= 500)
        {
            fprintf(stderr, "[slow] %s %lldms\n", name, dt);
            fflush(stderr);
        }
    }
};

// ---- 控制面发包预检/上下文裁剪(只动 application 层, JZNet 不碰) ----
static string stripJsonNewline(string s)
{
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
        s.pop_back();
    return s;
}

static string headBytesUtf8(const string &s, size_t n)
{
    if (s.size() <= n)
        return s;
    while (n > 0 && (s[n] & 0xC0) == 0x80)
        n--;
    return s.substr(0, n);
}

static bool isContextBoundary(const Json::Value &m)
{
    if (!m.isObject())
        return false;
    if (m.get("role", "").asString() == "user")
        return true;
    const string kind = m.get("kind", "").asString();
    return kind == "summary" || kind == "context_start" || kind == "conversation_start";
}

static void capContextMessage(Json::Value &msg, size_t textMax)
{
    if (!msg.isObject())
        return;

    if (msg.isMember("content"))
    {
        Json::Value &content = msg["content"];
        if (content.isString() && content.asString().size() > textMax)
        {
            string s = content.asString();
            content = headBytesUtf8(s, textMax) + "\n...[控制面单包上限, 已裁剪]";
        }
        else if (content.isArray())
        {
            for (Json::ArrayIndex i = 0; i < content.size(); i++)
            {
                Json::Value &part = content[i];
                if (!part.isObject())
                    continue;
                if (part.get("type", "").asString() == "text" && part.isMember("text"))
                {
                    string s = part["text"].asString();
                    if (s.size() > textMax)
                        part["text"] = headBytesUtf8(s, textMax) + "\n...[控制面单包上限, 已裁剪]";
                }
                else if (part.get("type", "").asString() == "image_url")
                {
                    part.removeMember("image_url");
                    part["type"] = "text";
                    part["text"] = "[控制面单包上限, 已省略图片]";
                }
            }
        }
    }

    if (!msg.isMember("tool_calls"))
        return;
    Json::Value &calls = msg["tool_calls"];
    if (calls.isArray())
    {
        for (Json::ArrayIndex i = 0; i < calls.size(); i++)
        {
            Json::Value &call = calls[i];
            if (!call.isObject() || !call.isMember("function") || !call["function"].isObject())
                continue;
            Json::Value &fn = call["function"];
            if (!fn.isMember("arguments"))
                continue;
            Json::Value &args = fn["arguments"];
            if (args.isString() && args.asString().size() > textMax)
            {
                string s = args.asString();
                args = headBytesUtf8(s, textMax) + "...}";   // 参数本身可能不是完整 JSON, 只求可发送
            }
        }
    }
}

static bool buildTaskSubmitPack(const string &taskId, const Json::Value &cliBody,
                                const Json::Value &ctx, JZHubPackTaskSubmit &submit)
{
    Json::Value body(cliBody);
    if (ctx.isObject())
        body["context"] = ctx;
    submit.taskId = taskId;
    submit.encoding = 0;
    string plain = stripJsonNewline(Json::FastWriter().write(body));
    hubPackZipEncode(plain, submit.body, submit.encoding);
    return hubPackSendable(&submit);
}

// 上下文压缩只影响"发给模型的这一段": ctx 文件保留完整历史(聊天记录要用),
// 真正交给 cli 之前从**最后一条 summary** 开始截(摘要在内, 它前面的是被压掉的旧消息)。
// 没有 summary 就整体照发。
static void trimHistoryToLastSummary(Json::Value &ctx)
{
    if (!ctx.isObject())
        return;
    Json::Value &hist = ctx["history"];
    if (!hist.isArray())
        return;
    Json::ArrayIndex last = 0;
    bool found = false;
    for (Json::ArrayIndex i = 0; i < hist.size(); i++)
    {
        if (hist[i].isObject() && hist[i].get("kind", "").asString() == "summary")
        {
            last = i;
            found = true;
        }
    }
    if (!found || last == 0)
        return;
    Json::Value cut(Json::arrayValue);
    for (Json::ArrayIndex i = last; i < hist.size(); i++)
        cut.append(hist[i]);
    hist = cut;
}

// 让「继续」在 10MB 单包上限内也能送出去: 先按完整轮次丢最老的, 再裁大字段。
// 返回 false 时调用方给 413, 绝不把超限包交给 JZNetClient::sendPacket。
static bool fitTaskSubmitContext(Json::Value &ctx, const Json::Value &cliBody,
                                 const string &taskId,
                                 JZHubPackTaskSubmit &submit, string &error)
{
    if (buildTaskSubmitPack(taskId, cliBody, ctx, submit))
        return true;

    if (!ctx.isObject() || !ctx["history"].isArray() || ctx["history"].empty())
    {
        error = "任务载荷超过控制面单包上限("
                + to_string(JZNetPackManager::maxPackSize() / (1024 * 1024)) + "MB), 请缩短指令或图片";
        return false;
    }

    const Json::Value hist = ctx["history"];
    for (Json::ArrayIndex i = 1; i < hist.size(); i++)
    {
        if (!isContextBoundary(hist[i]))
            continue;
        Json::Value cut(Json::arrayValue);
        for (Json::ArrayIndex j = i; j < hist.size(); j++)
            cut.append(hist[j]);
        Json::Value tryCtx(ctx);
        tryCtx["history"] = cut;
        if (buildTaskSubmitPack(taskId, cliBody, tryCtx, submit))
        {
            jzInfo() << "续会话上下文超过控制面包上限, 丢弃最早的 " << i << " 条历史";
            ctx = tryCtx;
            return true;
        }
    }

    // 单轮里就有巨型工具输出: 逐级裁剪正文/参数, 保住消息骨架和角色配对。
    for (size_t limit = 32 * 1024; limit >= 1024; limit /= 2)
    {
        Json::Value tryCtx(ctx);
        Json::Value &hist2 = tryCtx["history"];
        for (Json::ArrayIndex i = 0; i < hist2.size(); i++)
            capContextMessage(hist2[i], limit);
        if (buildTaskSubmitPack(taskId, cliBody, tryCtx, submit))
        {
            jzInfo() << "续会话上下文超过控制面包上限, 正文裁到 " << limit << " 字节/条";
            ctx = tryCtx;
            return true;
        }
    }

    error = "任务载荷超过控制面单包上限("
            + to_string(JZNetPackManager::maxPackSize() / (1024 * 1024)) + "MB), 请新建会话或缩短指令/图片";
    return false;
}

// ===========================================================================
// 单个被管理 agent 的运行实例
// ===========================================================================
struct JZCliServerAgentInstance
{
    JZCliServerAgentConfig cfg;
    JZInprocCli *inproc = nullptr;                     // 源码引用: 一台 cli = 一个线程
    JZCliAgentState state = JZCliAgent_Stopped;
    int64_t startTime = 0;
    int64_t stopTime = 0;

    mutable JZMutex mutex;          // 保护 logs / state(线程写, HTTP 线程读)
    std::deque<std::string> logs;   // 启停阶段的少量记录(运行日志走 JZLog 的 Agent.log)
    size_t logMax = 200;
};

// ---------------------------------------------------------------------------
// 路由处理器(把 HTTP 请求转交给 JZAgentCliServer 的对应方法)
// ---------------------------------------------------------------------------
class DashboardHandler : public JZHttpHandler
{
public:
    explicit DashboardHandler(JZAgentCliServer *s) : m_s(s) {}
    void handle(JZHttpServerClient *c, const JZHttpRequest &r) override { m_s->onDashboard(c, r); }
private:
    JZAgentCliServer *m_s;
};

class ApiStatusHandler : public JZHttpHandler
{
public:
    explicit ApiStatusHandler(JZAgentCliServer *s) : m_s(s) {}
    void handle(JZHttpServerClient *c, const JZHttpRequest &r) override { m_s->onApiStatus(c, r); }
private:
    JZAgentCliServer *m_s;
};

class ApiAgentsHandler : public JZHttpHandler
{
public:
    explicit ApiAgentsHandler(JZAgentCliServer *s) : m_s(s) {}
    void handle(JZHttpServerClient *c, const JZHttpRequest &r) override { m_s->onApiAgents(c, r); }
private:
    JZAgentCliServer *m_s;
};

class ApiAgentLogsHandler : public JZHttpHandler
{
public:
    explicit ApiAgentLogsHandler(JZAgentCliServer *s) : m_s(s) {}
    void handle(JZHttpServerClient *c, const JZHttpRequest &r) override { m_s->onApiAgentLogs(c, r); }
private:
    JZAgentCliServer *m_s;
};

class ApiAgentActionHandler : public JZHttpHandler
{
public:
    ApiAgentActionHandler(JZAgentCliServer *s, const string &a) : m_s(s), m_action(a) {}
    void handle(JZHttpServerClient *c, const JZHttpRequest &r) override
    {
        m_s->onApiAgentAction(c, r, m_action);
    }
private:
    JZAgentCliServer *m_s;
    string m_action;   // start / stop / restart
};

// 控制面: 提交任务(POST /api/tasks) / 取消任务(POST /api/tasks/cancel)
class ApiTasksHandler : public JZHttpHandler
{
public:
    explicit ApiTasksHandler(JZAgentCliServer *s) : m_s(s) {}
    void handle(JZHttpServerClient *c, const JZHttpRequest &r) override
    {
        // 同一个路径两种方法: GET = 历史列表, POST = 新建任务
        if (r.methodString() == "GET")
            m_s->onApiTaskList(c, r);
        else
            m_s->onApiTasks(c, r);
    }
private:
    JZAgentCliServer *m_s;
};

class ApiTaskDetailHandler : public JZHttpHandler
{
public:
    explicit ApiTaskDetailHandler(JZAgentCliServer *s) : m_s(s) {}
    void handle(JZHttpServerClient *c, const JZHttpRequest &r) override { m_s->onApiTaskDetail(c, r); }
private:
    JZAgentCliServer *m_s;
};

// 往前翻事件页("加载更多")
class ApiTaskEventsHandler : public JZHttpHandler
{
public:
    explicit ApiTaskEventsHandler(JZAgentCliServer *s) : m_s(s) {}
    void handle(JZHttpServerClient *c, const JZHttpRequest &r) override { m_s->onApiTaskEvents(c, r); }
private:
    JZAgentCliServer *m_s;
};

class ApiAgentDetailHandler : public JZHttpHandler
{
public:
    explicit ApiAgentDetailHandler(JZAgentCliServer *s) : m_s(s) {}
    void handle(JZHttpServerClient *c, const JZHttpRequest &r) override { m_s->onApiAgentDetail(c, r); }
private:
    JZAgentCliServer *m_s;
};

class ApiAgentForgetHandler : public JZHttpHandler
{
public:
    explicit ApiAgentForgetHandler(JZAgentCliServer *s) : m_s(s) {}
    void handle(JZHttpServerClient *c, const JZHttpRequest &r) override { m_s->onApiAgentForget(c, r); }
private:
    JZAgentCliServer *m_s;
};

// 静态页: 一个页面一个文件(server_web/<name>), 不再是一个页面塞所有东西
class PageHandler : public JZHttpHandler
{
public:
    PageHandler(JZAgentCliServer *s, const std::string &file) : m_s(s), m_file(file) {}
    void handle(JZHttpServerClient *c, const JZHttpRequest &r) override { m_s->servePage(c, r, m_file); }
private:
    JZAgentCliServer *m_s;
    std::string m_file;
};

class ApiTaskCancelHandler : public JZHttpHandler
{
public:
    explicit ApiTaskCancelHandler(JZAgentCliServer *s) : m_s(s) {}
    void handle(JZHttpServerClient *c, const JZHttpRequest &r) override { m_s->onApiTaskCancel(c, r); }
private:
    JZAgentCliServer *m_s;
};

class ApiTaskDeleteHandler : public JZHttpHandler
{
public:
    explicit ApiTaskDeleteHandler(JZAgentCliServer *s) : m_s(s) {}
    void handle(JZHttpServerClient *c, const JZHttpRequest &r) override { m_s->onApiTaskDelete(c, r); }
private:
    JZAgentCliServer *m_s;
};

class ApiTaskSubscribeHandler : public JZHttpHandler
{
public:
    explicit ApiTaskSubscribeHandler(JZAgentCliServer *s) : m_s(s) {}
    void handle(JZHttpServerClient *c, const JZHttpRequest &r) override { m_s->onApiTaskSubscribe(c, r); }
private:
    JZAgentCliServer *m_s;
};

// 网页常连(下行): /api/events 一条推全部(任务事件 + agent 快照 + 删除通知)
class ApiEventsHandler : public JZHttpHandler
{
public:
    explicit ApiEventsHandler(JZAgentCliServer *s) : m_s(s) {}
    void handle(JZHttpServerClient *c, const JZHttpRequest &r) override { m_s->onApiEvents(c, r); }
private:
    JZAgentCliServer *m_s;
};

class ApiAgentCardHandler : public JZHttpHandler
{
public:
    explicit ApiAgentCardHandler(JZAgentCliServer *s) : m_s(s) {}
    void handle(JZHttpServerClient *c, const JZHttpRequest &r) override { m_s->onApiAgentCard(c, r); }
private:
    JZAgentCliServer *m_s;
};

// 目录浏览器(网页上切换到某个工作目录): 只列 server 本机的目录。
// 托管模式下 cli 都是本机子进程, 文件系统和 server 是同一个。
class ApiFsHandler : public JZHttpHandler
{
public:
    explicit ApiFsHandler(JZAgentCliServer *s) : m_s(s) {}
    void handle(JZHttpServerClient *c, const JZHttpRequest &r) override { m_s->onApiFs(c, r); }
private:
    JZAgentCliServer *m_s;
};

// 文件内容(原始字节), 见 onApiFsRaw 的注释
class ApiFsRawHandler : public JZHttpHandler
{
public:
    explicit ApiFsRawHandler(JZAgentCliServer *s) : m_s(s) {}
    void handle(JZHttpServerClient *c, const JZHttpRequest &r) override { m_s->onApiFsRaw(c, r); }
private:
    JZAgentCliServer *m_s;
};

// 在工作区里新建目录/空文件(网页文件树的写入口)
class ApiFsCreateHandler : public JZHttpHandler
{
public:
    explicit ApiFsCreateHandler(JZAgentCliServer *s) : m_s(s) {}
    void handle(JZHttpServerClient *c, const JZHttpRequest &r) override { m_s->onApiFsCreate(c, r); }
private:
    JZAgentCliServer *m_s;
};

class ApiFsRenameHandler : public JZHttpHandler
{
public:
    explicit ApiFsRenameHandler(JZAgentCliServer *s) : m_s(s) {}
    void handle(JZHttpServerClient *c, const JZHttpRequest &r) override { m_s->onApiFsRename(c, r); }
private:
    JZAgentCliServer *m_s;
};

class ApiFsDeleteHandler : public JZHttpHandler
{
public:
    explicit ApiFsDeleteHandler(JZAgentCliServer *s) : m_s(s) {}
    void handle(JZHttpServerClient *c, const JZHttpRequest &r) override { m_s->onApiFsDelete(c, r); }
private:
    JZAgentCliServer *m_s;
};

// 主机视图: 一次拿全「每个 agent + 它用过的目录」
class ApiProjectsHandler : public JZHttpHandler
{
public:
    explicit ApiProjectsHandler(JZAgentCliServer *s) : m_s(s) {}
    void handle(JZHttpServerClient *c, const JZHttpRequest &r) override { m_s->onApiProjects(c, r); }
private:
    JZAgentCliServer *m_s;
};

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------
static void respondJson(JZHttpServerClient *client, const JZHttpRequest &req, int code, const Json::Value &v,
                        const JZHttpCookie *cookie = nullptr)
{
    JZHttpRespone resp;
    resp.setStatusCode(code);

    // 错误响应统一成 {"error": "..."}: 以前 4xx/5xx 有的回对象、有的回裸字符串
    // (Json::Value("目录不存在: ...")), 而裸字符串在 JSON 里就是个 string ——
    // 前端取 j.error 永远是 undefined, 网页上只剩一句"HTTP 404", 现场根本不知道
    // 缺的是哪个路径/哪个参数。在这里收口, 所有接口一次性统一。
    Json::Value body = v;
    if (code >= 400 && v.isString())
    {
        body = Json::Value(Json::objectValue);
        body["error"] = v;
    }
    resp.setJson(Json::FastWriter().write(body));
    if (cookie)
        resp.setCookie(*cookie);
    client->response(resp, req);
}

// 有副作用的接口只接受 POST。
// 为什么: 浏览器能被第三方页面用 <a href> / <meta refresh> / location.href 指使, 带着
// 受害者的 cookie 发一次**顶层 GET** —— 那是 CSRF, 不是"缺 token", 所以 token 校验拦不住。
// 而跨站 POST 在 SameSite=Lax|Strict 下都带不上 cookie, 于是"方法必须是 POST"从协议层
// 把这条路堵死, 和 isCrossSiteRequest(来源校验)互为兜底(一个管旧浏览器, 一个管新浏览器)。
// 参数仍可以用 query 传(POST /api/tasks/cancel?id=x 完全合法) —— 限制的是**方法**, 不是取参方式。
static bool requirePost(JZHttpServerClient *client, const JZHttpRequest &req)
{
    if (req.method() == JZHttpRequest::POST)
        return true;

    JZHttpRespone resp;
    resp.setStatusCode(405);
    resp.setHeader("Allow", "POST");
    resp.setJson(Json::FastWriter().write(
        Json::Value("该接口只接受 POST(收到 " + req.methodString() + ")")));
    client->response(resp, req);
    return false;
}

// 浏览器到服务端那一跳是不是 https —— 决定 cookie 要不要带 Secure。
//   直连: 后端开了 TLS 就是 https;
//   前面挂 nginx 时: 后端是明文, 看 X-Forwarded-Proto(多级代理可能是 "https, http", 取第一段)。
static bool isClientHttps(const JZHttpRequest &req, bool backendTls)
{
    if (backendTls)
        return true;

    string proto = req.header("X-Forwarded-Proto");
    size_t comma = proto.find(',');
    if (comma != string::npos)
        proto = proto.substr(0, comma);

    size_t b = proto.find_first_not_of(" \t");
    size_t e = proto.find_last_not_of(" \t");
    if (b == string::npos)
        return false;
    proto = proto.substr(b, e - b + 1);
    for (size_t i = 0; i < proto.size(); i++)
    {
        if (proto[i] >= 'A' && proto[i] <= 'Z')
            proto[i] = (char)(proto[i] + 32);
    }
    return proto == "https";
}

// 只做 query 取值需要的编码: 保留 unreserved 字符, 其余按 %XX
static string urlEncodeComponent(const string &s)
{
    static const char *hex = "0123456789ABCDEF";
    string out;
    for (size_t i = 0; i < s.size(); i++)
    {
        unsigned char c = (unsigned char)s[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~')
            out += (char)c;
        else
        {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0x0F];
        }
    }
    return out;
}

// 手机浏览器 → 手机页; ?ui=mobile|desktop 可强制覆盖(便于真机/自动化验证与书签)。
// UA 只用来选「哪份 HTML 外壳」: 数据层和 /api 接口完全是同一套。
static bool isMobileBrowser(const JZHttpRequest &req)
{
    const string force = req.queryParam("ui");
    if (force == "mobile") return true;
    if (force == "desktop") return false;

    string ua = req.userAgent();
    for (char &c : ua)
        if (c >= 'A' && c <= 'Z') c = char(c + 32);
    static const char *keys[] = {
        "mobile", "android", "iphone", "ipad", "ipod",
        "windows phone", "blackberry", "opera mini", "iemobile", "webos"
    };
    for (const char *k : keys)
        if (ua.find(k) != string::npos) return true;
    return false;
}

static void respondText(JZHttpServerClient *client, const JZHttpRequest &req, int code,
                        const string &text, const string &ctype = "text/plain; charset=utf-8",
                        const string &cacheControl = string(),
                        const string &vary = string())
{
    JZHttpRespone resp;
    resp.setStatusCode(code);
    resp.setContentType(ctype);
    if (!cacheControl.empty())
        resp.setHeader("Cache-Control", cacheControl);
    // 同一 URL 按 UA 返回不同 HTML: 中间有代理/缓存时必须声明 Vary
    if (!vary.empty())
        resp.setHeader("Vary", vary);
    resp.setBody(text);
    client->response(resp, req);
}

// 任务回包是**异步**的(等 cli 跑完), 那时请求对象早没了 —— 只能用 buffer 版本。
// ---------------------------------------------------------------------------
// /api/* 的应用层鉴权(见 JZCliServerHttpConfig::token)
//
// 为什么只护 /api/*: 静态页 / app.js / css 是"拿到也没用"的东西, 而登录框本身要靠它们
// 才能加载出来 —— 把静态资源也护上, 用户永远看不到可以输入 token 的界面。
//
// token 来源优先级:
//   1) cookie  jz_token=...   ← 网页把输入框里的值写进 cookie, 浏览器之后**每个请求**都带
//   2) header  X-JZ-Token: ...
//   3) header  Authorization: Bearer ...
// ⚠ **不支持** ?token=<...>: URL 会进浏览器历史 / Referer / 反向代理的 access log,
//   HTTPS 只保护传输过程, 保护不了这些记录。脚本/调试请改用上面两个头。
// ---------------------------------------------------------------------------
static bool tokenEquals(const string &a, const string &b)
{
    // 常量时间比较: 别让"第几个字节开始不同"从响应时间上泄漏出去
    if (a.size() != b.size())
        return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); i++)
        diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0;
}

// 从请求里取 token(cookie > query > 头); 取不到返回空串
static string tokenFromRequest(const JZHttpRequest &req)
{
    string tk = req.cookie("jz_token");
    if (tk.empty())
        tk = req.header("X-JZ-Token");
    if (tk.empty())
    {
        string auth = req.header("Authorization");
        if (auth.size() > 7 && str_toLower(auth.substr(0, 7)) == "bearer ")
            tk = auth.substr(7);
    }
    return tk;
}

// ---------------------------------------------------------------------------
// 客户端 IP —— 这是**部署策略**, 归应用层; 框架的 req.remoteAddr() 永远只报 TCP 对端。
//   http.trustProxy=false(默认): 直接用对端地址。
//   http.trustProxy=true: 前面确实挂了 nginx 之类、且监听端口只对反代可达, 才取
//     X-Forwarded-For 首段(多级代理是逗号列表, 最左 = 最初的客户端)。
// 首段必须"看着像 IP"才采用: 这个头完全由客户端控制, 不校验形态就能塞几 KB 垃圾进来,
// 而它会成为 m_authFails 的 key(按 IP 的失败计数) —— 等于把内存放大倍数交给对方。
// ---------------------------------------------------------------------------
static bool looksLikeIpAddress(const string &s)
{
    if (s.empty() || s.size() > 45)   // IPv6 文本形式最长 45 字符
        return false;
    for (size_t i = 0; i < s.size(); i++)
    {
        const char c = s[i];
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                        (c >= 'A' && c <= 'F') || c == '.' || c == ':';
        if (!ok)
            return false;
    }
    return true;
}

static string clientIpOf(const JZHttpRequest &req, const string &peerIp, bool trustProxy)
{
    if (!trustProxy)
        return peerIp;

    const string xff = req.header("X-Forwarded-For");
    if (xff.empty())
        return peerIp;

    const size_t comma = xff.find(',');
    const string first = str_trimmed((comma == string::npos) ? xff : xff.substr(0, comma));
    return looksLikeIpAddress(first) ? first : peerIp;
}

// ---------------------------------------------------------------------------
// CSRF 判定: 这个 /api/* 请求是不是"别的站点"发起的。
//
// 为什么 token 正确还不够: token 存在 cookie 里, 浏览器对**顶层 GET 导航**会自动带上它
// (SameSite=Lax 的例外正是顶层 GET)。于是第三方页面里一句
//   <meta http-equiv="refresh" content="0;url=https://admin:8088/api/agents/stop?name=a1">
// 就能借受害者的登录态停 agent / 删任务 —— 攻击者全程不需要知道 token。
// 前端 cookie 已改 SameSite=Strict, 这里再从服务端判一道, 因为:
//   · Strict 是"浏览器愿意配合"才生效(旧浏览器不认识这个属性, 会退回 None/Lax);
//   · token 也可能走 X-JZ-Token / Authorization 头(脚本), 那条路径没有 cookie 策略 ——
//     服务端统一判来源, 两种来源一个口径。
//
// 判定顺序:
//   1) Sec-Fetch-Site —— 现代浏览器必发, 只有 "cross-site" 算攻击
//      (same-origin / same-site / none=地址栏/书签 都放行);
//   2) 没有它(旧浏览器): 用 Origin / Referer 的 host 与 Host 比;
//   3) 两者都没有(curl / 脚本 / 隐私模式): 放行 —— 非浏览器请求没有"自动携带 cookie"
//      这回事, 带得出 token 就是有权限, 也是自动化脚本的既有用法。
// ---------------------------------------------------------------------------
static bool isCrossSiteRequest(const JZHttpRequest &req)
{
    const string sfs = str_toLower(str_trimmed(req.header("Sec-Fetch-Site")));
    if (!sfs.empty())
        return sfs == "cross-site";

    const string host = str_toLower(str_trimmed(req.header("Host")));
    static const char *kSourceHeaders[] = {"Origin", "Referer"};
    for (const char *name : kSourceHeaders)
    {
        const string v = str_toLower(str_trimmed(req.header(name)));
        if (v.empty())
            continue;
        const size_t scheme = v.find("://");
        if (scheme == string::npos)
            continue;      // "null"(sandbox iframe) 之类: 判不出来, 交给 token
        const size_t slash = v.find('/', scheme + 3);
        const string src = v.substr(scheme + 3,
                                    (slash == string::npos ? v.size() : slash) - scheme - 3);
        if (!src.empty() && src != host)
            return true;
        if (!src.empty())
            return false;  // 同源, 不用再看下一个头
    }
    return false;
}

// 异步回包拿不到当次 req, 由 PendingTask/PendingFs 记下 Accept-Encoding 传进来。
static void respondJsonAsync(JZHttpServerClient *client, int code, const Json::Value &v,
                             const string &acceptEncoding = string())
{
    if (!client)
        return;
    JZHttpRespone resp;
    resp.setStatusCode(code);
    resp.setJson(Json::FastWriter().write(v));
    resp.applyContentEncoding(acceptEncoding);
    client->response(resp.toByteArray());
}

static bool parseJsonText(const string &text, Json::Value &out)
{
    Json::Reader reader;
    return reader.parse(text, out) && !out.isNull();
}

// ---------------------------------------------------------------------------
// A2A v1.0 在管理台侧的落地辅助
//   - 内部状态词(running/success/...)一个不改, 只在输出时映射成 TaskState;
//   - events / artifacts / history 在内存里存的是"一行 JSON 字符串", 落盘时组装成数组;
//   - SSE 用 chunked 分块(与 agent_cli 的 /events 同口径), 浏览器与非浏览器客户端都能逐帧读。
// ---------------------------------------------------------------------------
// 只有一套状态: A2A TaskState。cli 上报的 success/failed/cancelled 只在边界映射一次。
static const char *kStateSubmitted = "TASK_STATE_SUBMITTED";
static const char *kStateWorking = "TASK_STATE_WORKING";
static const char *kStateCompleted = "TASK_STATE_COMPLETED";
static const char *kStateFailed = "TASK_STATE_FAILED";
static const char *kStateCanceled = "TASK_STATE_CANCELED";
static const char *kStateRejected = "TASK_STATE_REJECTED";
static const char *kStateUnknown = "TASK_STATE_UNSPECIFIED";

// 任务来源: 管理台的"任务日志"只列 user, agent 内部派活(内置 agent 的工具)不算一条用户任务。
static const char *kOriginUser = "user";
static const char *kOriginAgent = "agent";

static bool isWorkingState(const string &s)
{
    return s == kStateSubmitted || s == kStateWorking ||
           s == "TASK_STATE_INPUT_REQUIRED" || s == "TASK_STATE_AUTH_REQUIRED";
}

static string stateFromResult(const string &status)
{
    if (status == "success")   return kStateCompleted;
    if (status == "cancelled") return kStateCanceled;
    if (status == "rejected")  return kStateRejected;
    if (status == "failed" || status == "timeout") return kStateFailed;
    return kStateUnknown;      // lost / 未知: 结果不确定, 不能当成 completed
}

static string isoTime(long long ms)
{
    if (ms <= 0)
        return string();
    time_t sec = (time_t)(ms / 1000);
    tm tmv;
#ifdef _WIN32
    gmtime_s(&tmv, &sec);
#else
    gmtime_r(&sec, &tmv);
#endif
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmv);
    return string(buf);
}

// A2A SendMessage 的 Message -> cli /task 需要的纯文本指令(只在这里转换一次)
static string messageText(const Json::Value &body)
{
    const Json::Value &msg = body.get("message", Json::Value::null);
    if (msg.isObject())
    {
        const Json::Value &parts = msg.get("parts", Json::Value::null);
        if (parts.isArray())
        {
            string text;
            for (Json::ArrayIndex i = 0; i < parts.size(); ++i)
            {
                const string one = parts[i].get("text", "").asString();
                if (!text.empty() && !one.empty()) text += "\n";
                text += one;
            }
            return text;
        }
    }
    return string();
}

static string toJsonLine(const Json::Value &v)
{
    string s = Json::FastWriter().write(v);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
        s.pop_back();
    return s;
}

static Json::Value jsonStringArray(const vector<string> &items)
{
    Json::Value arr(Json::arrayValue);
    for (const auto &s : items)
    {
        Json::Value v;
        arr.append(parseJsonText(s, v) ? v : Json::Value(s));
    }
    return arr;
}

// 只取 [begin, end) 这一段
static Json::Value jsonStringArrayFrom(const vector<string> &items, size_t begin, size_t end)
{
    Json::Value arr(Json::arrayValue);
    if (end > items.size())
        end = items.size();
    for (size_t i = begin; i < end; ++i)
    {
        Json::Value v;
        arr.append(parseJsonText(items[i], v) ? v : Json::Value(items[i]));
    }
    return arr;
}

// 一页事件的起点: 末尾 limit 条, 再往前对齐到最近的一条 user_msg
// (最多多带 60 条, 免得"单轮超长"那种任务一次吐全量)。
static size_t eventPageBegin(const vector<string> &items, size_t end, size_t limit)
{
    if (end <= limit)
        return 0;
    const size_t begin = end - limit;
    const size_t guard = begin > 60 ? begin - 60 : 0;
    static const string kUser = "\"user_msg\"";
    for (size_t i = begin; i > guard; --i)
    {
        if (items[i - 1].find(kUser) != string::npos)
            return i - 1;
    }
    return begin;
}

static vector<string> stringsFromJson(const Json::Value &arr)
{
    vector<string> out;
    if (!arr.isArray())
        return out;
    for (Json::ArrayIndex i = 0; i < arr.size(); ++i)
    {
        const Json::Value &v = arr[i];
        out.push_back(v.isString() ? v.asString() : toJsonLine(v));
    }
    return out;
}

// hub 包里的 data 可能是 JSON 对象/字符串, 也可能被上游裁过而不合法; 一律归成 Value,
// 文本太长就截断(事件序列要落库, 一条巨型日志会撑爆 payload_json)。
static Json::Value looseJson(const string &text)
{
    if (text.empty())
        return Json::Value(Json::objectValue);
    Json::Value v;
    if (parseJsonText(text, v))
        return v;
    return Json::Value(text.size() > 4000 ? text.substr(0, 4000) + "…" : text);
}

static string makeSseFrame(const string &event, const string &payload)
{
    string frame;
    if (!event.empty())
        frame += "event: " + event + "\n";
    string data;
    data.reserve(payload.size());
    for (char c : payload)
        data += (c == '\n' || c == '\r') ? ' ' : c;   // SSE 帧里 data 必须单行
    frame += "data: " + data + "\n\n";
    return frame;
}

static string sseChunk(const string &frame)
{
    char head[32];
    snprintf(head, sizeof(head), "%x\r\n", (unsigned)frame.size());
    return string(head) + frame + "\r\n";
}

// 网页专用的"瘦"Task 帧: /tasks 详情页把 SSE 当纯信号, 收到帧自己 GET /api/tasks/detail。
// 完整 Task 快照随 events 累积能到几 MB, 单帧会撑爆 3MB send buffer 被 net 层关连,
// 而 EventSource 自动重连后的首帧又是同一个大帧 —— 于是变成反复拉大包的重连风暴。
// 网页订阅(?slim=1)只发 id/state/eventSeq; 不带 slim 的 A2A 调用方仍拿完整 Task, 语义不变。
static Json::Value makeSlimTaskSignal(const string &taskId, const string &state, Json::Int64 eventSeq)
{
    Json::Value j(Json::objectValue);
    j["id"] = taskId;
    j["state"] = state;
    j["eventSeq"] = eventSeq;
    return j;
}

static const char *stateString(JZCliAgentState s)
{
    switch (s)
    {
        case JZCliAgent_Running:  return "running";
        case JZCliAgent_Starting: return "starting";
        default:                  return "stopped";
    }
}

static Json::Value agentToJson(JZCliServerAgentInstance *inst)
{
    Json::Value j;
    j["name"] = inst->cfg.name;
    j["enabled"] = inst->cfg.enabled;
    j["args"] = Json::arrayValue;
    for (const auto &a : inst->cfg.args)
        j["args"].append(a);
    j["running"] = (inst->state == JZCliAgent_Running);
    j["state"] = stateString(inst->state);
    j["startTime"] = inst->startTime;
    j["stopTime"] = inst->stopTime;
    {
        JZMutexLocker lock(&inst->mutex);
        j["logLines"] = (int)inst->logs.size();
    }
    return j;
}

// ===========================================================================
// JZAgentCliServer
// ===========================================================================
JZAgentCliServer::JZAgentCliServer(JZObject *parent)
    : JZObject(parent)
    , m_taskStore(JZPlatform::applicationDirPath() + "/data")
{
    m_taskStore.setPayloadBuilder([this](TaskRecord &r) {
        ensureTaskDetail(r);
        Json::Value j = taskJson(r, -1);
        // 聊天记录不随任务落盘: 唯一真源是会话上下文(ctx 存档, 增量写), 读的时候用
        // eventsFromContext() 重建。运行中的事件只在内存里供 SSE/网页实时用。
        // 这样就不会再出现"任务库快照停在几十条、和上下文对不上"的两份数据。
        if (j["metadata"].isObject())
        {
            j["metadata"]["events"] = Json::Value(Json::arrayValue);
            j["metadata"]["eventsTotal"] = 0;
            j["metadata"]["eventsOffset"] = 0;
        }
        return Json::FastWriter().write(j);
    });
}

JZAgentCliServer::~JZAgentCliServer()
{
    for (auto &kv : m_agents)
    {
        // inproc: 先让线程退出(join 之后才安全 delete, 否则线程还在跑对象就没了)
        if (kv.second->inproc)
        {
            kv.second->inproc->stop();
            delete kv.second->inproc;
            kv.second->inproc = nullptr;
        }
        delete kv.second;
    }
    m_agents.clear();
}

// 整文件读成文本(证书/私钥/CA 都是 PEM 文本); 读不到返回 false
static bool readTextFile(const string &path, string &out)
{
    out.clear();
    ifstream ifs(path, ios::binary);
    if (!ifs)
        return false;
    stringstream ss;
    ss << ifs.rdbuf();
    out = ss.str();
    return !out.empty();
}

bool JZAgentCliServer::init(const JZCliServerConfig &config, std::string &error)
{
    m_config = config;

    // HTTPS 证书: 页面/API 默认走 TLS(只加密, 不认证客户端), 所以这里先把服务器证书读进内存。
    // 读不到直接失败(而不是退回明文 HTTP): 静默降级等于把"起停 agent + 下发任务"的执行权
    // 暴露成裸 HTTP, 现场根本看不出来。
    m_httpCertPem.clear();
    m_httpKeyPem.clear();
    if (m_config.http.sslEnabled)
    {
        if (!readTextFile(m_config.http.certFile, m_httpCertPem))
        {
            error = "HTTPS 已启用, 但读不到服务器证书: " + m_config.http.certFile;
            return false;
        }
        if (!readTextFile(m_config.http.keyFile, m_httpKeyPem))
        {
            error = "HTTPS 已启用, 但读不到服务器私钥: " + m_config.http.keyFile;
            return false;
        }
    }

    // 为每个 agent 配置登记实例。真正的 cli 线程由 spawnAgent 在 hub 起来之后再起
    // (cli 一启动就接管控制面, hub 没起会白报一轮连接失败)。
    for (const auto &ac : m_config.agents)
    {
        auto *inst = new JZCliServerAgentInstance();
        inst->cfg = ac;
        m_agents[ac.name] = inst;
    }

    return true;
}

bool JZAgentCliServer::start(std::string &error)
{
    m_startTime = JZDateTime::currentMSecsSinceEpoch();

    // 两个端口都是"必须起来"的: 控制面挂了管不了远端 cli, 管理台挂了人什么都看不见。
    // 失败就直接返回, 由 main 打印原因并退出 —— 不带着半死不活的状态进事件循环。
    if (!startHub(error))    // 控制面(JZNet): 等 agent_cli 出站连进来
        return false;
    if (!startHttp(error))   // 后台网页 + 管理 API
        return false;

    for (const auto &kv : m_agents)
        if (kv.second->cfg.enabled)
            spawnAgent(kv.first);
    return true;
}

// ---------------------------------------------------------------------------
// 控制面(hub): agent_cli 出站连进来(JZNet + token)
//   cli 是 client, 本进程是 server —— 内网机器不需要端口暴露/端口映射,
//   server 也不需要知道 cli 的地址(见 docs/agent-hub-design.md §2)。
// ---------------------------------------------------------------------------
bool JZAgentCliServer::startHub(std::string &error)
{
    JZAgentHubPacketInit();

    m_hub = new JZNetServer(this);
    if (!m_config.hub.token.empty())
        m_hub->setToken(m_config.hub.token);

    connect(&m_hub->sigAccept, this, [this]()
    {
        // 握手完成才会 emit sigAccept; 这里只把待处理队列排空(否则会一直堆着)
        while (true)
        {
            uint64_t conn = m_hub ? m_hub->nextPendingConnection() : 0;
            if (conn == 0)
                break;
            jzInfo().noquote().nospace() << "控制面: 新连接 " << conn << " (等 hello)";
        }
    });
    connect(&m_hub->sigPacketReceive, this, [this](uint64_t conn, JZNetPackPtr pack)
    {
        onHubPacket(conn, pack);
    });
    connect(&m_hub->sigDisconnected, this, [this](uint64_t conn)
    {
        onHubDisconnected(conn);
    });

    // 心跳回收定时器(5s 一次; 判定阈值 20s 见 reapDeadAgents)
    m_reapTimer = new JZTimer(this);
    connect(&m_reapTimer->sigTimeout, this, [this]()
    {
        reapDeadAgents();     // 心跳超时的 agent
        reapStuckTasks();     // 长时间没有任何事件的任务
        reapStuckFs();        // 远端文件操作卡住(agent 掉线/正在跑任务)不能把 HTTP 挂死
        pruneAuthFails();     // 鉴权失败记录(过期就丢, 别被扫描器撑大)
        // 顺便当 SSE 心跳: 网页靠"20s 收不到任何帧"判断下行断了(前端 EVT_LAST)
        broadcastAgentEvent();
    });
    m_reapTimer->start(5000);

    if (!m_hub->listen(m_config.hub.bind, m_config.hub.port))
    {
        error = "控制面监听失败: " + m_config.hub.bind + ":" + to_string(m_config.hub.port)
                + " (端口被占用, 或 bind 地址不属于本机网卡)";
        jzWarning() << error.c_str();
        return false;
    }
    jzInfo() << "控制面已启动(JZNet):" << m_config.hub.bind << ":" << m_config.hub.port
             << (m_config.hub.token.empty() ? "  token=(无)" : "  token=(已启用)");
    return true;
}

bool JZAgentCliServer::sendPack(uint64_t conn, const JZNetPack *pack)
{
    if (!m_hub)
        return false;
    if (!hubPackSendable(pack))
    {
        jzWarning() << "控制面包超过单包上限, 断开连接(conn=" << conn
                    << ", type=" << pack->packetType() << ")";
        m_hub->closeClient(conn);
        return false;
    }
    m_hub->sendPacket(conn, pack);
    return true;
}

void JZAgentCliServer::onHubPacket(uint64_t conn, JZNetPackPtr pack)
{
    if (!pack || !m_hub)
        return;

    const int type = pack->packetType();
    const int64_t now = JZDateTime::currentMSecsSinceEpoch();

    if (type == HubPack_hello)
    {
        JZHubPackHello *hello = (JZHubPackHello *)pack.get();
        if (hello->agentId.empty())
        {
            JZHubPackHelloAck ack;
            ack.ok = false;
            ack.error = "hello 缺 agentId";
            ack.serverTimeMs = now;
            sendPack(conn, &ack);
            m_hub->closeClient(conn);
            return;
        }

        // 同一个 agentId 又连上来(进程重启/网络切换): 让旧连接让位, 否则管理台会出现两行
        auto old = m_connByAgent.find(hello->agentId);
        if (old != m_connByAgent.end() && old->second != conn)
        {
            const uint64_t oldConn = old->second;
            // 先走统一下线收尾, 再关旧连接; 直接 erase 会让旧任务永远停在 running。
            dropAgent(oldConn, "被同 agentId 的新连接替换");
            m_hub->closeClient(oldConn);
        }

        JZCliServerOnlineAgent agent;
        agent.conn = conn;
        agent.agentId = hello->agentId;
        agent.name = hello->name;
        agent.version = hello->version;
        agent.rootDir = hello->rootDir;
        agent.project = hello->project;
        agent.modelName = hello->modelName;
        agent.contextWindow = hello->contextWindow;
        agent.modelWindows = hello->modelWindows;
        agent.nodes = hello->nodes;
        agent.lastSeen = now;
        m_onlineByConn[conn] = agent;
        m_connByAgent[agent.agentId] = conn;

        // 记进"曾经连过"的档案(离线后列表仍然认得出这台)
        KnownAgent known;
        known.agentId = agent.agentId;
        known.name = agent.name;
        known.version = agent.version;
        known.rootDir = agent.rootDir;
        known.project = agent.project;
        known.modelName = agent.modelName;
        known.lastSeen = now;
        upsertKnown(known);
        broadcastAgentEvent();      // 网页常连: 立刻推一份 agent 列表

        JZHubPackHelloAck ack;
        ack.ok = true;
        ack.acceptTasks = true;
        ack.serverTimeMs = now;
        sendPack(conn, &ack);

        jzInfo().noquote().nospace() << "cli 上线: " << agent.agentId << " (" << agent.name
                                     << " v" << agent.version << ")  conn=" << conn;
        return;
    }

    auto it = m_onlineByConn.find(conn);
    if (it == m_onlineByConn.end())
    {
        jzWarning().noquote().nospace() << "控制面: 未 hello 的包(type=" << type << "), 已丢弃";
        return;
    }
    JZCliServerOnlineAgent &agent = it->second;
    agent.lastSeen = now;

    if (type == HubPack_heartbeat)
    {
        JZHubPackHeartbeat *hb = (JZHubPackHeartbeat *)pack.get();
        agent.busy = hb->busy;
        agent.round = hb->round;
        agent.taskId = hb->taskId;
        agent.nodes = hb->nodes;
        return;
    }

    if (type == HubPack_taskEvent)
    {
        JZHubPackTaskEvent *ev = (JZHubPackTaskEvent *)pack.get();
        string data;
        if (!hubPackZipDecode(ev->data, ev->encoding, data))
            return;
        if (ev->type == "context_history")
        {
            // 模型上下文增量: 只更新 ctx 文件, 不进 Task.events/SSE 过程流。
            Json::Value msg;
            if (parseJsonText(data, msg))
                applyContextMessage(ev->taskId, msg);
            return;
        }
        agent.lastEventType = ev->type;
        agent.lastEventData = data;
        recordEvent(ev->taskId, ev->type, data);   // A2A: 事件进 Task.events + SSE 广播
        return;
    }

    if (type == HubPack_taskEnd)
    {
        JZHubPackTaskEnd *end = (JZHubPackTaskEnd *)pack.get();
        agent.busy = false;
        agent.taskId.clear();

        string resultText;
        if (!hubPackZipDecode(end->result, end->encoding, resultText))
        {
            resultText.clear();
            jzWarning() << "taskEnd 解压失败: " << end->taskId;
        }
        Json::Value result;
        if (!parseJsonText(resultText, result) || !result.isObject())
        {
            result = Json::Value(Json::objectValue);
            result["status"] = "failed";
            result["error"] = "cli 返回的任务结果不是合法 JSON";
        }
        // 先落记录: 即使提交方早就断了(超时/异步模式), 历史也要留全。
        // 上下文已由 context_history 增量落盘, task_end 不再带全量 context。
        recordEnd(end->taskId, result);

        auto pit = m_pendingTasks.find(end->taskId);
        if (pit == m_pendingTasks.end())
        {
            jzInfo().noquote().nospace() << "任务结束(无提交方在等): " << end->taskId;
            return;
        }

        PendingTask pt = pit->second;
        m_pendingTasks.erase(pit);

        // 同步等待方拿到的也是 A2A Task, 与 GetTask / SubscribeToTask 同一形状
        TaskRecord *rec = findTask(end->taskId);
        respondJsonAsync(pt.client, 200, rec ? taskJson(*rec) : Json::Value(Json::objectValue),
                         pt.acceptEncoding);
        jzInfo().noquote().nospace() << "任务完成: " << end->taskId << "  agent=" << pt.agentId
                                     << "  state=" << (rec ? rec->state : string(kStateUnknown));
        return;
    }

    // 远端文件操作的回包: 找到挂起中的 HTTP 请求, 把结果写回去
    if (type == HubPack_fsRes)
    {
        JZHubPackFsRes *res = (JZHubPackFsRes *)pack.get();
        onHubFsRes(*res);
        return;
    }
}

// ---------------------------------------------------------------------------
// 远端文件操作: 把 op 转给 cli, 等它回包(fsRes)再回 HTTP
// ---------------------------------------------------------------------------
bool JZAgentCliServer::fsForward(const string &agentId, const string &op, const Json::Value &body,
                                 JZHttpServerClient *client, const string &kind, string &error,
                                 const string &acceptEncoding)
{
    error.clear();

    auto ai = m_connByAgent.find(agentId);
    if (ai == m_connByAgent.end() || !m_hub)
    {
        error = "没有在线的 agent: " + agentId;
        return false;
    }
    JZHubPackFsReq req;
    req.reqId = "fs" + to_string(++m_fsSeq) + "-" + to_string(JZPlatform::getTickCountMs());
    req.op = op;
    req.body = Json::FastWriter().write(body);

    PendingFs p;
    p.agentId = agentId;
    p.client = client;
    p.kind = kind;
    p.acceptEncoding = acceptEncoding;
    p.deadline = JZPlatform::getTickCountMs() + 8000;   // 远端 cli 可能正在跑任务, 给 8s
    m_pendingFs[req.reqId] = p;

    // 提交方提前断开: 只丢回包通道, 别把悬空指针留到回包那一刻
    if (client)
    {
        connect(static_cast<JZSignal<JZObject*>*>(&client->sigDestroyed), this, [this](JZObject *obj)
        {
            for (auto &kv : m_pendingFs)
                if (kv.second.client == static_cast<JZHttpServerClient *>(obj))
                    kv.second.client = nullptr;
        });
    }

    jzInfo().noquote().nospace() << "远端文件操作: " << op << " -> " << agentId
                                 << "  req=" << req.reqId;
    sendPack(ai->second, &req);
    return true;
}

int JZAgentCliServer::fsRoute(JZHttpServerClient *client, const JZHttpRequest &req,
                              const string &agentId, const string &op, const Json::Value &body,
                              const string &kind)
{
    // 没指定 agent: 文件就在 server 本机, 直接读盘
    if (agentId.empty())
        return 0;

    // 指定了 agent 就必须在**它那台机器**上执行: 离线不能悄悄回落到 server 本机的同名路径
    if (m_connByAgent.count(agentId) == 0)
    {
        respondJson(client, req, 503, Json::Value("agent 不在线: " + agentId));
        return 2;
    }

    string error;
    if (!fsForward(agentId, op, body, client, kind, error, req.header("Accept-Encoding")))
    {
        respondJson(client, req, 503, Json::Value(error));
        return 2;
    }
    return 1;                        // 已转发, 等 fsRes 回包
}

void JZAgentCliServer::onHubFsRes(const JZHubPackFsRes &res)
{
    auto it = m_pendingFs.find(res.reqId);
    if (it == m_pendingFs.end())
        return;                     // 已经超时回过 504, 或提交方断了
    PendingFs p = it->second;
    m_pendingFs.erase(it);
    if (!p.client)
        return;

    string body;
    if (!hubPackZipDecode(res.body, res.encoding, body))
    {
        Json::Value e(Json::objectValue);
        e["error"] = "cli 返回的文件内容解压失败";
        respondJsonAsync(p.client, 502, e, p.acceptEncoding);
        return;
    }

    if (!res.ok)
    {
        Json::Value e(Json::objectValue);
        e["error"] = res.error.empty() ? string("远端文件操作失败") : res.error;
        respondJsonAsync(p.client, 404, e, p.acceptEncoding);
        return;
    }

    if (p.kind == "raw")
    {
        // 与本地 /api/fs/raw 完全一样的响应形状(前端按 X-File-Size 判断截断)
        JZHttpRespone resp;
        resp.setStatusCode(200);
        resp.setContentType("application/octet-stream");
        resp.setHeader("Cache-Control", "no-store");
        resp.setHeader("X-File-Size", to_string((long long)res.size));
        resp.setHeader("X-File-Mtime", to_string((long long)res.mtime));
        if (res.truncated)
            resp.setHeader("X-File-Truncated", "1");
        resp.setBody(body);
        resp.applyContentEncoding(p.acceptEncoding);
        p.client->response(resp.toByteArray());
        return;
    }

    Json::Value j;
    if (!parseJsonText(body, j))
    {
        Json::Value e(Json::objectValue);
        e["error"] = "cli 返回的文件结果不是合法 JSON";
        respondJsonAsync(p.client, 502, e, p.acceptEncoding);
        return;
    }
    respondJsonAsync(p.client, 200, j, p.acceptEncoding);
}

// 超时清扫(由 5s 巡检调用): 远端 cli 掉线/卡住时不能把 HTTP 请求一直挂着
void JZAgentCliServer::reapStuckFs()
{
    if (m_pendingFs.empty())
        return;

    const int64_t now = JZPlatform::getTickCountMs();
    vector<string> dead;
    for (auto &kv : m_pendingFs)
        if (kv.second.deadline > 0 && now > kv.second.deadline)
            dead.push_back(kv.first);

    for (const auto &id : dead)
    {
        auto it = m_pendingFs.find(id);
        if (it == m_pendingFs.end())
            continue;
        JZHttpServerClient *c = it->second.client;
        const string acceptEncoding = it->second.acceptEncoding;
        m_pendingFs.erase(it);
        if (!c)
            continue;
        Json::Value e(Json::objectValue);
        e["error"] = "远端 agent 响应超时(可能正在跑任务或已掉线)";
        respondJsonAsync(c, 504, e, acceptEncoding);
    }
}

// 一台 agent 下线(两条来路: socket 断开 / 心跳超时), 收口逻辑只写一份
void JZAgentCliServer::dropAgent(uint64_t conn, const string &reason)
{
    auto it = m_onlineByConn.find(conn);
    if (it == m_onlineByConn.end())
        return;   // 未 hello 就断了

    const string agentId = it->second.agentId;
    m_onlineByConn.erase(it);
    auto ai = m_connByAgent.find(agentId);
    if (ai != m_connByAgent.end() && ai->second == conn)
        m_connByAgent.erase(ai);

    jzWarning().noquote().nospace() << "cli 掉线: " << agentId << " (" << reason << ")";
    failPendingTasksOf(agentId, "agent " + agentId + " " + reason + ", 任务结果未知");

    // 还在非终态(异步模式没有提交方在等)的任务也要标死: 否则历史里会永远停在 working
    vector<string> running;
    for (const auto &kv : m_tasks)
        if (kv.second.agentId == agentId && isWorkingState(kv.second.state))
            running.push_back(kv.first);
    for (const auto &id : running)
        recordFail(id, kStateUnknown, "agent " + agentId + " " + reason + ", 结果未知");

    broadcastAgentEvent();          // 网页常连: 立刻推一份 agent 列表
}

void JZAgentCliServer::onHubDisconnected(uint64_t conn)
{
    dropAgent(conn, "连接断开");
}

// 心跳回收: 控制面不能只靠 socket 断开 —— 拔网线/休眠/被防火墙丢包时 TCP 还是"通"的,
// 但对端早就不干活了。cli 每 5s 一次心跳, 超过 20s(4 次没到)就按掉线处理。
void JZAgentCliServer::reapDeadAgents()
{
    const int64_t now = JZDateTime::currentMSecsSinceEpoch();
    vector<uint64_t> dead;
    for (const auto &kv : m_onlineByConn)
        if (now - kv.second.lastSeen > 20000)
            dead.push_back(kv.first);

    for (uint64_t conn : dead)
    {
        dropAgent(conn, "心跳超时(>20s)");
        if (m_hub)
            m_hub->closeClient(conn);   // 顺手把死连接关掉, 免得一直占 fd(已断开时是 no-op)
    }
}

// 任务"无响应"巡检: 距**最后一条事件**超过 timeoutMs 就判失败。
// 用 JZPlatform::getTickCountMs() 直接比时间, 不给每个任务单独开 JZTimer ——
// 流式回复每秒几十条事件, 每来一条 stop/start 一个定时器既浪费又容易踩重入限制。
// 粒度 = m_reapTimer 的 5s(15 分钟量级差 5s 无所谓)。
void JZAgentCliServer::reapStuckTasks()
{
    if (m_pendingTasks.empty())
        return;
    const int64_t now = JZPlatform::getTickCountMs();

    vector<string> stuck;
    for (const auto &kv : m_pendingTasks)
    {
        const PendingTask &pt = kv.second;
        if (pt.timeoutMs > 0 && now - pt.lastEventMs > pt.timeoutMs)
            stuck.push_back(pt.taskId);
    }

    for (const auto &taskId : stuck)
    {
        auto pit = m_pendingTasks.find(taskId);
        if (pit == m_pendingTasks.end())
            continue;
        PendingTask pt = pit->second;
        m_pendingTasks.erase(pit);

        const int secs = pt.timeoutMs / 1000;
        const std::string human =
            secs >= 60 ? (to_string(secs / 60) + " 分钟") : (to_string(secs) + " 秒");
        recordFail(taskId, kStateFailed, "任务无响应(" + human + "没有任何事件)");

        auto ai = m_connByAgent.find(pt.agentId);
        if (ai != m_connByAgent.end() && m_hub)
        {
            JZHubPackTaskCancel cancel;
            cancel.taskId = taskId;
            sendPack(ai->second, &cancel);
        }

        // 回包与 GetTask 同形状(504 只是 HTTP 状态, body 仍是 Task)
        TaskRecord *rec = findTask(taskId);
        respondJsonAsync(pt.client, 504, rec ? taskJson(*rec) : Json::Value(Json::objectValue),
                         pt.acceptEncoding);
    }
}

void JZAgentCliServer::failPendingTasksOf(const string &agentId, const string &error)
{
    for (auto it = m_pendingTasks.begin(); it != m_pendingTasks.end(); )
    {
        if (it->second.agentId != agentId)
        {
            ++it;
            continue;
        }
        PendingTask pt = it->second;
        it = m_pendingTasks.erase(it);

        recordFail(pt.taskId, kStateUnknown, error);   // 对提交方是 502; 历史必须保留"结果未知"
        TaskRecord *rec = findTask(pt.taskId);
        respondJsonAsync(pt.client, 502, rec ? taskJson(*rec) : Json::Value(Json::objectValue),
                         pt.acceptEncoding);
    }
}

Json::Value JZAgentCliServer::onlineAgentsJson() const
{
    Json::Value arr(Json::arrayValue);
    for (const auto &kv : m_onlineByConn)
    {
        const JZCliServerOnlineAgent &a = kv.second;
        Json::Value j(Json::objectValue);
        j["agentId"] = a.agentId;
        j["name"] = a.name;
        j["version"] = a.version;
        j["rootDir"] = a.rootDir;
        j["project"] = a.project;
        j["modelName"] = a.modelName;
        j["contextWindow"] = a.contextWindow;
        {
            Json::Value mw;
            if (parseJsonText(a.modelWindows, mw) && mw.isObject())
                j["modelWindows"] = mw;
            else
                j["modelWindows"] = Json::Value(Json::objectValue);
        }
        j["busy"] = a.busy;
        j["round"] = a.round;
        j["taskId"] = a.taskId;
        j["acceptTasks"] = a.acceptTasks;
        j["lastSeen"] = (Json::Int64)a.lastSeen;
        j["lastEventType"] = a.lastEventType;
        j["lastEvent"] = a.lastEventData;   // JSON 字符串(已按包上限裁过)
        Json::Value nodes;
        if (parseJsonText(a.nodes, nodes))
            j["nodes"] = nodes;
        else
            j["nodes"] = Json::Value(Json::arrayValue);
        arr.append(j);
    }

    return arr;
}

// ---------------------------------------------------------------------------
// 任务记录: server 侧历史(SQLite 实现在 JZAgentTaskStore)
// ---------------------------------------------------------------------------
void JZAgentCliServer::loadTaskStore()
{
    const string dir = JZPlatform::applicationDirPath() + "/data";
    m_ctxDir = dir + "/ctx";
    JZDir dataDir;
    dataDir.mkpath(m_ctxDir);

    string error;
    if (!m_taskStore.open(&error)) {
        jzWarning() << "任务库打开失败:" << error;
        return;
    }

    vector<TaskRecord> records;
    if (!m_taskStore.load(records, &error)) {
        jzWarning() << "任务库载入失败:" << error;
        return;
    }
    for (TaskRecord &r : records) {
        if (m_tasks.find(r.taskId) == m_tasks.end())
            m_taskOrder.push_back(r.taskId);
        m_tasks[r.taskId] = r;
    }
    while (m_taskOrder.size() > 500) {
        m_tasks.erase(m_taskOrder.front());
        m_taskOrder.erase(m_taskOrder.begin());
    }
    set<string> liveIds;
    for (const auto &kv : m_tasks)
        liveIds.insert(kv.first);
    m_taskStore.prune(liveIds);
    jzInfo().noquote().nospace() << "任务记录: 载入 " << records.size()
                                 << " 条摘要 (" << m_taskStore.dbFile() << ")";
}

void JZAgentCliServer::ensureTaskDetail(TaskRecord &r) const
{
    m_taskStore.ensureDetail(r);
    // 任务记录不保存聊天记录(见构造里的 payloadBuilder): 内存里没有就从会话上下文重建。
    if (r.events.empty())
    {
        Json::Value evs = eventsFromContext(r);
        for (const auto &e : evs)
            r.events.push_back(toJsonLine(e));
        r.eventSeq = (int)r.events.size();
        r.detailLoaded = true;
    }
}

// 聊天记录 = 会话上下文(ctx 存档)里属于这条任务的那一段。
// ctx 里每条消息带 taskId(applyContextMessage 盖章), 据此切分; at 是展示用时间戳。
Json::Value JZAgentCliServer::eventsFromContext(const TaskRecord &r) const
{
    Json::Value out(Json::arrayValue);
    if (r.taskId.empty())
        return out;

    Json::Value ctx;
    auto it = m_contextCache.find(r.contextId);
    if (it != m_contextCache.end())
        ctx = it->second;
    else if (!r.contextId.empty())
        ctx = loadContextFile(r.contextId);
    if (!ctx.isObject())
        return out;
    const Json::Value &hist = ctx["history"];
    if (!hist.isArray())
        return out;

    auto pushEvent = [&out](const char *type, Json::Value data, Json::Int64 at)
    {
        Json::Value e(Json::objectValue);
        e["seq"] = (int)out.size() + 1;
        e["at"] = at;
        e["type"] = type;
        e["data"] = data;
        out.append(e);
    };

    // 任务生命周期(submitted/task_start/task_end)不合成: 用户消息本身就是一轮的起点,
    // 任务状态在 status/metadata 里, 聊天记录只由历史消息推出来。
    for (const auto &m : hist)
    {
        if (!m.isObject())
            continue;
        if (m.get("taskId", "").asString() != r.taskId)
            continue;   // 别的任务的消息(同一条会话的上一轮)
        const string kind = m.get("kind", "").asString();
        if (kind == "summary")
            continue;   // 压缩摘要是内部消息, 不进界面
        const string role = m.get("role", "").asString();
        const Json::Int64 at = m.get("at", (Json::Int64)0).asInt64();

        // 思考 / 错误各自是一条独立历史(role=thinking / error), 直接映射成一条事件
        if (role == "thinking")
        {
            Json::Value d(Json::objectValue);
            d["text"] = m.get("content", "").asString();
            pushEvent("thinking", d, at);
            continue;
        }
        if (role == "error")
        {
            Json::Value d(Json::objectValue);
            d["text"] = m.get("content", "").asString();
            pushEvent("error", d, at);
            continue;
        }

        if (role == "user")
        {
            Json::Value d(Json::objectValue);
            d["text"] = m.get("content", "").asString();
            pushEvent("user_msg", d, at);
            continue;
        }
        if (role == "assistant")
        {
            const string content = m.get("content", "").asString();
            if (!content.empty())
            {
                Json::Value d(Json::objectValue);
                d["text"] = content;
                pushEvent("reply", d, at);
            }
            const Json::Value &calls = m["tool_calls"];
            if (calls.isArray())
            {
                for (const auto &tc : calls)
                {
                    Json::Value d(Json::objectValue);
                    d["id"] = tc.get("id", "").asString();
                    const Json::Value &fn = tc["function"];
                    d["name"] = fn.get("name", "").asString();
                    d["args"] = fn.get("arguments", "").asString();
                    pushEvent("tool_call", d, at);
                }
            }
            continue;
        }
        if (role == "tool")
        {
            Json::Value d(Json::objectValue);
            d["id"] = m.get("tool_call_id", "").asString();
            d["result"] = m.get("content", "").asString();
            pushEvent("tool_result", d, at);
            continue;
        }
    }

    return out;
}

void JZAgentCliServer::upsertTaskRecord(TaskRecord &r)
{
    m_taskStore.upsert(r);
}

void JZAgentCliServer::pruneTaskStore()
{
    set<string> liveIds;
    for (const auto &kv : m_tasks)
        liveIds.insert(kv.first);
    m_taskStore.prune(liveIds);
}

// ---------------------------------------------------------------------------
// 会话上下文存档: data/ctx/<contextId>.json, 一个会话一个文件, 每次整体覆盖
//   只留"这条会话当前状态" —— 续聊够用, 不做任意任务的历史回溯, 也就不存在
//   "第 N 条任务把前 N 轮再存一遍" 的体积膨胀。
// ---------------------------------------------------------------------------
string JZAgentCliServer::contextFile(const string &contextId) const
{
    // contextId 可能是客户端传的, 不能直接拿来当文件名: 只留安全字符, 改过就补个哈希防撞
    string safe;
    for (char c : contextId)
    {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                        || c == '-' || c == '_' || c == '.';
        safe.push_back(ok ? c : '_');
    }
    if (safe.empty())
        safe = "ctx";
    if (safe != contextId)
        safe += "-" + to_string(std::hash<string>()(contextId));
    return m_ctxDir + "/" + safe + ".json";
}

void JZAgentCliServer::saveContextFile(const string &contextId, const Json::Value &ctx)
{
    if (m_ctxDir.empty() || contextId.empty() || !ctx.isObject())
        return;
    const string path = contextFile(contextId);
    ofstream ofs(path, ios::binary | ios::trunc);
    if (!ofs)
    {
        jzWarning() << "会话上下文写不进去:" << path;
        return;
    }
    ofs << Json::FastWriter().write(ctx);
}

Json::Value JZAgentCliServer::loadContextFile(const string &contextId) const
{
    if (m_ctxDir.empty() || contextId.empty())
        return Json::Value();
    const string path = contextFile(contextId);
    ifstream ifs(path, ios::binary);
    if (!ifs)
        return Json::Value();      // 没存过(新会话 / 老记录 / 已清理): 从零开始
    std::stringstream ss;
    ss << ifs.rdbuf();
    Json::Value j;
    if (!parseJsonText(ss.str(), j) || !j.isObject())
        return Json::Value();
    return j;
}

void JZAgentCliServer::applyContextMessage(const string &taskId, const Json::Value &msg)
{
    TaskRecord *r = findTask(taskId);
    if (!r || r->contextId.empty() || !msg.isObject() || m_ctxDir.empty())
        return;

    const string &id = r->contextId;
    Json::Value ctx;
    auto it = m_contextCache.find(id);
    if (it != m_contextCache.end())
        ctx = it->second;
    else
        ctx = loadContextFile(id);
    if (!ctx.isObject())
        ctx = Json::Value(Json::objectValue);
    if (!ctx.isMember("history") || !ctx["history"].isArray())
        ctx["history"] = Json::Value(Json::arrayValue);
    if (!ctx.isMember("usage") || !ctx["usage"].isObject())
    {
        ctx["usage"] = Json::Value(Json::objectValue);
        ctx["usage"]["prompt_tokens"] = 0;
        ctx["usage"]["completion_tokens"] = 0;
        ctx["usage"]["total_tokens"] = 0;
        ctx["usage"]["cached_tokens"] = 0;
    }

    // 摘要只按序 append, **不清空**旧历史: ctx 是聊天记录的唯一真源, 旧消息要留着给界面;
    // 压缩对模型的影响在取上下文时处理(见 trimHistoryToLastSummary)。
    Json::Value &hist = ctx["history"];
    // 从上下文重建聊天记录(eventsFromContext)要按任务切分: 一条会话跨多轮任务,
    // 所以每条消息都盖上所属 taskId(at 由 cli 写入)。
    Json::Value stamped = msg;
    stamped["taskId"] = taskId;
    hist.append(stamped);

    saveContextFile(id, ctx);
    m_contextCache[id] = ctx;
}

void JZAgentCliServer::dropContextIfUnused(const string &contextId)
{
    if (contextId.empty())
        return;
    for (const auto &kv : m_tasks)
        if (kv.second.contextId == contextId)
            return;      // 这条会话还有任务, 上下文留着
    m_contextCache.erase(contextId);
    JZDir().remove(contextFile(contextId));
}

void JZAgentCliServer::saveTaskRecord(TaskRecord &r)
{
    upsertTaskRecord(r);
}

void JZAgentCliServer::rewriteTaskStore()
{
    pruneTaskStore();
}

// ---------------------------------------------------------------------------
// 曾经连过的 agent: agents.json(整文件重写, 数量很小)
// ---------------------------------------------------------------------------
void JZAgentCliServer::loadAgentStore()
{
    m_agentFile = JZPlatform::applicationDirPath() + "/data/agents.json";

    ifstream ifs(m_agentFile, ios::binary);
    if (!ifs)
        return;

    stringstream ss;
    ss << ifs.rdbuf();
    Json::Value root;
    if (!parseJsonText(ss.str(), root) || !root.isArray())
    {
        jzWarning() << "agents.json 解析失败, 先忽略:" << m_agentFile;
        return;
    }

    for (Json::ArrayIndex i = 0; i < root.size(); ++i)
    {
        const Json::Value &j = root[i];
        KnownAgent a;
        a.agentId = j.get("agentId", "").asString();
        if (a.agentId.empty())
            continue;
        a.name = j.get("name", "").asString();
        a.version = j.get("version", "").asString();
        a.rootDir = j.get("rootDir", "").asString();
        a.project = j.get("project", "").asString();
        a.modelName = j.get("modelName", "").asString();
        a.firstSeen = (long long)j.get("firstSeen", 0).asInt64();
        a.lastSeen = (long long)j.get("lastSeen", 0).asInt64();
        a.connects = j.get("connects", 0).asInt();
        m_known[a.agentId] = a;
    }
    jzInfo().noquote().nospace() << "agent 记录: 载入 " << m_known.size() << " 台(含离线)";
}

void JZAgentCliServer::saveAgentStore()
{
    if (m_agentFile.empty())
        return;
    Json::Value arr(Json::arrayValue);
    for (const auto &kv : m_known)
    {
        const KnownAgent &a = kv.second;
        Json::Value j(Json::objectValue);
        j["agentId"] = a.agentId;
        j["name"] = a.name;
        j["version"] = a.version;
        j["rootDir"] = a.rootDir;
        j["project"] = a.project;
        j["modelName"] = a.modelName;
        j["firstSeen"] = (Json::Int64)a.firstSeen;
        j["lastSeen"] = (Json::Int64)a.lastSeen;
        j["connects"] = a.connects;
        arr.append(j);
    }
    ofstream ofs(m_agentFile, ios::binary | ios::trunc);
    if (!ofs)
    {
        jzWarning() << "agent 记录写不进去:" << m_agentFile;
        return;
    }
    ofs << arr.toStyledString();
}

void JZAgentCliServer::upsertKnown(const KnownAgent &info)
{
    auto it = m_known.find(info.agentId);
    if (it == m_known.end())
    {
        KnownAgent a = info;
        if (a.firstSeen == 0)
            a.firstSeen = a.lastSeen;
        a.connects = 1;
        m_known[a.agentId] = a;
    }
    else
    {
        KnownAgent &a = it->second;
        if (a.firstSeen == 0)
            a.firstSeen = info.firstSeen ? info.firstSeen : a.lastSeen;
        if (!info.name.empty())
            a.name = info.name;
        if (!info.version.empty())
            a.version = info.version;
        if (!info.rootDir.empty())
            a.rootDir = info.rootDir;
        if (!info.project.empty())
            a.project = info.project;
        if (!info.modelName.empty())
            a.modelName = info.modelName;
        a.lastSeen = info.lastSeen;
        a.connects = a.connects + 1;
    }
    saveAgentStore();
}

// 列表页要的: 全部见过的 agent, 在线/离线都在里面(在线优先, 其次按最近一次在线排序)
Json::Value JZAgentCliServer::knownAgentsJson() const
{
    // Json::Value 的迭代器不是随机访问的, 不能直接 std::sort -> 先攒进 vector
    std::vector<Json::Value> items;
    items.reserve(m_known.size() + 1);

    for (const auto &kv : m_known)
    {
        const KnownAgent &a = kv.second;
        Json::Value j(Json::objectValue);
        j["agentId"] = a.agentId;
        j["name"] = a.name;
        j["version"] = a.version;
        j["rootDir"] = a.rootDir;
        j["project"] = a.project;
        j["modelName"] = a.modelName;
        j["firstSeen"] = (Json::Int64)a.firstSeen;
        j["connects"] = a.connects;
        j["builtin"] = false;

        auto ci = m_connByAgent.find(a.agentId);
        const bool online = (ci != m_connByAgent.end());
        j["online"] = online;
        j["lastSeen"] = (Json::Int64)a.lastSeen;
        j["busy"] = false;
        j["taskId"] = "";
        j["round"] = 0;
        j["lastEventType"] = "";
        j["nodes"] = Json::Value(Json::arrayValue);

        if (online)
        {
            auto oi = m_onlineByConn.find(ci->second);
            if (oi != m_onlineByConn.end())
            {
                const JZCliServerOnlineAgent &o = oi->second;
                j["busy"] = o.busy;
                j["taskId"] = o.taskId;
                j["round"] = o.round;
                j["lastSeen"] = (Json::Int64)o.lastSeen;
                j["lastEventType"] = o.lastEventType;
                Json::Value nodes;
                if (parseJsonText(o.nodes, nodes))
                    j["nodes"] = nodes;
            }
        }
        items.push_back(j);
    }

    std::sort(items.begin(), items.end(), [](const Json::Value &x, const Json::Value &y)
    {
        const bool xo = x.get("online", false).asBool();
        const bool yo = y.get("online", false).asBool();
        if (xo != yo)
            return xo;   // 在线的排前面
        return x.get("lastSeen", (Json::Int64)0).asInt64() > y.get("lastSeen", (Json::Int64)0).asInt64();
    });

    Json::Value arr(Json::arrayValue);
    for (const auto &v : items)
        arr.append(v);
    return arr;
}

void JZAgentCliServer::recordCreate(const string &taskId, const string &agentId, const Json::Value &body,
                                    const string &origin, const string &parentTaskId)
{
    TaskRecord r;
    r.taskId = taskId;
    r.agentId = agentId;
    r.prompt = messageText(body);
    r.verifyCmd = body.get("verifyCmd", "").asString();
    r.origin = origin.empty() ? string(kOriginUser) : origin;
    r.parentTaskId = parentTaskId;
    r.state = kStateSubmitted;
    r.contextId = body.get("contextId", "").asString();
    if (r.contextId.empty())
        r.contextId = "ctx-" + taskId;   // 每条指令默认自成一个上下文; 前端可显式沿用
    // project: 任务归属哪个项目(来自 agent)。网页"全部/按项目筛选"都靠它。
    r.project = body.get("project", "").asString();
    if (r.project.empty())
        r.project = body.get("rootDir", "").asString();   // 切到哪个目录, 就归哪个 project
    if (r.project.empty())
    {
        auto ai = m_connByAgent.find(agentId);
        if (ai != m_connByAgent.end())
        {
            auto o = m_onlineByConn.find(ai->second);
            if (o != m_onlineByConn.end())
                r.project = o->second.project;
        }
    }
    r.startMs = r.statusMs = (long long)JZDateTime::currentMSecsSinceEpoch();

    // 请求里的 A2A Message 原样进 Task.history(补上 id / role / contextId)
    Json::Value msg = body.get("message", Json::Value(Json::objectValue));
    if (!msg.isObject())
        msg = Json::Value(Json::objectValue);
    msg["messageId"] = taskId + ":user";
    msg["role"] = "ROLE_USER";
    msg["taskId"] = taskId;
    msg["contextId"] = r.contextId;
    if (!msg.isMember("parts"))
    {
        Json::Value parts(Json::arrayValue);
        Json::Value part(Json::objectValue);
        part["text"] = r.prompt;
        parts.append(part);
        msg["parts"] = parts;
    }
    r.history.push_back(toJsonLine(msg));
    // 聊天区要知道"这轮用户说了什么": 每轮追加一条 user_msg(与 reply/tool 事件同序, 天然连续)
    {
        Json::Value um(Json::objectValue);
        um["text"] = r.prompt;
        appendTaskEvent(&r, "user_msg", um);
    }
    Json::Value submitted(Json::objectValue);
    submitted["agentId"] = agentId;
    submitted["state"] = kStateSubmitted;
    appendTaskEvent(&r, "submitted", submitted);

    if (m_tasks.find(taskId) == m_tasks.end())
        m_taskOrder.push_back(taskId);
    m_tasks[taskId] = r;
    while (m_taskOrder.size() > 500)
    {
        m_tasks.erase(m_taskOrder.front());
        m_taskOrder.erase(m_taskOrder.begin());
    }
    pruneTaskStore();
    saveTaskRecord(r);
}

// 「接着说」: 往已有任务里追加一轮(同一个 taskId / contextId), 事件与 A2A history 都往后长。
// 任务记录只此一条, 所以前端聊天区天然连续; 执行仍是新的一次 run。
void JZAgentCliServer::recordContinue(const string &taskId, const Json::Value &body)
{
    TaskRecord *r = findTask(taskId);
    if (!r)
        return;
    ensureTaskDetail(*r);
    flushOpenSegment(r);        // 上一轮最后那段先落, 别和新一轮搅在一起
    const long long now = (long long)JZDateTime::currentMSecsSinceEpoch();

    r->prompt = messageText(body);            // 侧栏/日志显示最新一句
    r->verifyCmd = body.get("verifyCmd", r->verifyCmd).asString();
    r->state = kStateSubmitted;               // 上一轮已收尾, 重新进入执行态
    r->error.clear();
    r->endMs = 0;
    r->statusMs = now;

    // A2A Task.history: 追加一条 user 消息(整条会话的往返都留在这里)
    Json::Value msg = body.get("message", Json::Value(Json::objectValue));
    if (!msg.isObject())
        msg = Json::Value(Json::objectValue);
    msg["messageId"] = taskId + ":user:" + to_string(r->history.size() + 1);
    msg["role"] = "ROLE_USER";
    msg["taskId"] = taskId;
    msg["contextId"] = r->contextId;
    if (!msg.isMember("parts"))
    {
        Json::Value parts(Json::arrayValue);
        Json::Value part(Json::objectValue);
        part["text"] = r->prompt;
        parts.append(part);
        msg["parts"] = parts;
    }
    r->history.push_back(toJsonLine(msg));

    Json::Value um(Json::objectValue);
    um["text"] = r->prompt;
    appendTaskEvent(r, "user_msg", um);

    Json::Value submitted(Json::objectValue);
    submitted["agentId"] = r->agentId;
    submitted["state"] = kStateSubmitted;
    appendTaskEvent(r, "submitted", submitted);
    saveTaskRecord(*r);
}

void JZAgentCliServer::recordEnd(const string &taskId, const Json::Value &result)
{
    TaskRecord *r = findTask(taskId);
    if (!r || !isWorkingState(r->state))
        return;   // timeout/lost 等 server 终态不能被迟到的 taskEnd 覆盖
    ensureTaskDetail(*r);
    flushOpenSegment(r);        // 收尾前把正在攒的 reply/thinking 段落成一条
    const string reply = result.get("agent_reply", "").asString();
    r->state = stateFromResult(result.get("status", "").asString());
    r->rounds = result.get("verify_rounds", 0).asInt();
    r->error = result.get("error", "").asString();
    r->usage = result.get("usage", Json::Value());
    r->usageTurn = taskUsageTurn(*r);   // 本次提交: 最后一次 user_msg 之后的用量
    r->endMs = r->statusMs = (long long)JZDateTime::currentMSecsSinceEpoch();

    // A2A: 结果放 Artifact, 不要用 Message 传输出
    if (!reply.empty())
    {
        Json::Value a(Json::objectValue);
        a["artifactId"] = "reply";
        a["name"] = "agent 回复";
        Json::Value parts(Json::arrayValue);
        Json::Value part(Json::objectValue);
        part["text"] = reply;
        parts.append(part);
        a["parts"] = parts;
        r->artifacts.push_back(toJsonLine(a));
    }
    const string verifyOut = result.get("last_verify_output", "").asString();
    if (!verifyOut.empty())
    {
        Json::Value a(Json::objectValue);
        a["artifactId"] = "verify";
        a["name"] = "验证输出";
        Json::Value parts(Json::arrayValue);
        Json::Value part(Json::objectValue);
        part["text"] = verifyOut;
        parts.append(part);
        a["parts"] = parts;
        r->artifacts.push_back(toJsonLine(a));
    }

    Json::Value tail(Json::objectValue);
    tail["state"] = r->state;
    tail["error"] = r->error;
    tail["verifyRounds"] = r->rounds;
    tail["artifacts"] = (int)r->artifacts.size();
    appendTaskEvent(r, "task_end", tail);
    saveTaskRecord(*r);

    // cli 不再把 task_end 当 taskEvent 发一遍; 终态信号在这里补, 内容只带尾巴。
    broadcastTaskEvent(taskId, "task_end", toJsonLine(tail), r->eventSeq);
    broadcastTaskEvent(taskId, "artifact_update",
                       toJsonLine(jsonStringArray(r->artifacts)), r->eventSeq);
    finishTaskStreams(taskId);   // 终态: 补完整 Task 快照后关流
}

void JZAgentCliServer::recordFail(const string &taskId, const string &state, const string &error)
{
    TaskRecord *r = findTask(taskId);
    if (!r || !isWorkingState(r->state))
        return;
    ensureTaskDetail(*r);
    flushOpenSegment(r);
    r->state = state;
    r->error = error;
    r->endMs = r->statusMs = (long long)JZDateTime::currentMSecsSinceEpoch();
    Json::Value tail(Json::objectValue);
    tail["state"] = state;
    tail["error"] = error;
    appendTaskEvent(r, "task_end", tail);
    saveTaskRecord(*r);
    broadcastTaskEvent(taskId, "task_end", toJsonLine(tail), r->eventSeq);
    finishTaskStreams(taskId);
}

void JZAgentCliServer::flushOpenSegment(TaskRecord *r)
{
    if (!r || r->openKind.empty())
        return;
    Json::Value e(Json::objectValue);
    e["seq"] = ++r->eventSeq;
    e["at"] = (Json::Int64)(r->openAt ? r->openAt : JZDateTime::currentMSecsSinceEpoch());
    e["type"] = r->openKind;
    Json::Value d(Json::objectValue);
    d["text"] = r->openText;
    e["data"] = d;
    r->events.push_back(toJsonLine(e));
    r->lastEventType = r->openKind;
    r->statusMs = (long long)JZDateTime::currentMSecsSinceEpoch();
    r->openKind.clear();
    r->openText.clear();
    r->openAt = 0;
    // 事件变了就必须让落盘快照失效, 见 appendTaskEvent 里的说明
    r->payloadJson.clear();
}

void JZAgentCliServer::appendTaskEvent(TaskRecord *r, const string &type, const Json::Value &data)
{
    if (!r)
        return;
    ensureTaskDetail(*r);

    // reply/thinking 是逐 token 来的: 攒成一整段再落一条, 否则 events 全是碎片。
    if (type == "reply" || type == "thinking")
    {
        const string text = data.isString() ? data.asString()
                          : (data.isObject() ? data.get("text", "").asString() : string());
        if (r->openKind == type)
        {
            // 整段攒着, 只在类型切换/收尾时落成一条事件: 历史(以及从历史重建的聊天记录)
            // 就是"每段一条"。生成过程中要看进度, 网页吃 SSE 帧里带的 chunk, 见
            // broadcastTaskEvent 里 status_update 的处理。
            r->openText += text;
            return;
        }
        flushOpenSegment(r);              // 上一段是另一种类型: 先落
        r->openKind = type;
        r->openText = text;
        r->openAt = JZDateTime::currentMSecsSinceEpoch();
        return;
    }

    flushOpenSegment(r);                  // 别的类型来了, 保证事件顺序
    Json::Value e(Json::objectValue);
    e["seq"] = ++r->eventSeq;
    e["at"] = (Json::Int64)JZDateTime::currentMSecsSinceEpoch();
    e["type"] = type;
    e["data"] = data;
    r->events.push_back(toJsonLine(e));
    r->lastEventType = type;
    r->statusMs = (long long)JZDateTime::currentMSecsSinceEpoch();
    // ⚠ payloadJson 是"落盘快照"的缓存, 只在 ensureDetail() 里由旧快照解析后清空。
    // 以前追加事件/历史时不失效 ⇒ upsert() 每次都把**第一次构造时**的 payload 再写一遍
    // (见 JZAgentTaskStore::upsert), 于是 DB 里的 events 永远停在二十几条, 进程一挂就
    // 只剩那份旧快照 —— 表现就是"重启后聊天记录被截断"。事件一变就让快照失效。
    r->payloadJson.clear();
}

void JZAgentCliServer::recordEvent(const string &taskId, const string &type, const string &data)
{
    TaskRecord *r = findTask(taskId);
    if (!r || !isWorkingState(r->state))
        return;
    if (r->state == kStateSubmitted && type != "submitted")
        r->state = kStateWorking;       // 第一个执行事件: submitted -> working

    // 无响应计时续期: 这条任务还在出事件, 说明它没死 —— 记下时间就行,
    // 判不判由 5s 巡检(reapStuckTasks)拿 tick 计数去比。
    auto pit = m_pendingTasks.find(taskId);
    if (pit != m_pendingTasks.end())
        pit->second.lastEventMs = JZPlatform::getTickCountMs();

    Json::Value v = looseJson(data);
    appendTaskEvent(r, type, v);
    // 推给 SSE 的是**这一条原始分片**(不能用 r->events.back(): reply/thinking 会合并,
    // back() 未必是刚来的这条; 新任务的第一条事件可能还没落, back() 会越界)。
    // 不做节流: 每条都推。网页直接把这帧当事件用(不再回拉 /api/tasks/events)。
    Json::Value wire(Json::objectValue);
    wire["seq"] = r->eventSeq;
    wire["at"] = (Json::Int64)JZDateTime::currentMSecsSinceEpoch();
    wire["type"] = type;
    wire["data"] = v;
    broadcastTaskEvent(taskId, "status_update", toJsonLine(wire), r->eventSeq);
    // 每 20 条 + 终态各存一次 SQLite; 中途 crash 只丢尾部过程。
    // 落盘前先把正在攒的 reply/thinking 落成一条, 否则文件里一直是"上一次 flush 的状态"。
    if (r->eventSeq % 20 == 0)
    {
        flushOpenSegment(r);
        saveTaskRecord(*r);
    }
}

// ---- SSE 订阅者 ----
void JZAgentCliServer::removeTaskStream(JZHttpServerClient *client)
{
    m_slimStreams.erase(client);
    for (auto it = m_taskStreams.begin(); it != m_taskStreams.end(); )
    {
        vector<JZHttpServerClient*> &v = it->second;
        v.erase(std::remove(v.begin(), v.end(), client), v.end());
        if (v.empty())
            it = m_taskStreams.erase(it);
        else
            ++it;
    }
}

void JZAgentCliServer::broadcastTaskEvent(const string &taskId, const string &event,
                                          const string &payload, int seq)
{
    // 网页常连(下行): 所有任务的事件都推一份, 帧里带 taskId/seq, 前端自己筛。
    // 这条连接不随任务结束关闭 —— 关流/重连就是"连接中断"的来源。
    if (!m_pageStreams.empty())
    {
        // 常连对网页只是"这个任务有新数据了"的信号, 但必须带 seq:
        // 前端只对当前显示的任务按 seq 拉增量, 不再反复拉整张任务列表。
        // 不推原始分片: 一条 tool_result 可以到 96KB。
        Json::Value w(Json::objectValue);
        w["taskId"] = taskId;
        w["event"] = event;
        w["seq"] = seq;
        if (TaskRecord *r = findTask(taskId))
        {
            w["state"] = r->state;
            w["lastEventType"] = r->lastEventType;
        }
        // 事件按"够小就整条推, 太大就推标题"发下去: 网页收到帧直接落进本地事件表,
        // 不再回拉 /api/tasks/events; 只有被截断的(truncated)才在用户点开时按 seq 取那一条。
        // thinking/reply 是逐 token 的分片, 额外带一份 chunk 给"边生成边显示"用。
        if (event == "status_update" && !payload.empty())
        {
            Json::Value ev;
            if (parseJsonText(payload, ev) && ev.isObject())
            {
                const string tp = ev.get("type", "").asString();
                w["type"] = tp;
                w["at"] = ev.get("at", (Json::Int64)0);
                Json::Value d = ev["data"];
                static const size_t kInlineMax = 4096;     // 超过这个字节数就不整条推
                static const size_t kPreviewBytes = 512;   // 推下去的预览长度(utf8 字节)
                const size_t bytes = Json::FastWriter().write(d).size();
                if (bytes > kInlineMax)
                {
                    // 只截"长文本"字段, 保留 name/id/ok/seq 这类结构信息
                    static const char *kLongFields[] = { "text", "result", "output", "args", "content" };
                    for (size_t i = 0; i < sizeof(kLongFields) / sizeof(kLongFields[0]); i++)
                    {
                        if (!d.isMember(kLongFields[i]) || !d[kLongFields[i]].isString())
                            continue;
                        string s = d[kLongFields[i]].asString();
                        if (s.size() > kPreviewBytes)
                        {
                            s.resize(kPreviewBytes);
                            // 别切坏多字节字符: 退到最后一个完整 utf8 起始字节
                            while (!s.empty() && ((unsigned char)s.back() & 0xC0) == 0x80)
                                s.pop_back();
                            d[kLongFields[i]] = s;
                        }
                    }
                    d["truncated"] = true;
                    d["full_bytes"] = (Json::Int64)bytes;
                    w["truncated"] = true;
                }
                w["data"] = d;
                if (tp == "thinking" || tp == "reply")
                {
                    const string text = d.isString() ? d.asString()
                                       : (d.isObject() ? d.get("text", "").asString() : string());
                    if (!text.empty())
                        w["chunk"] = text;
                }
            }
        }
        broadcastPageEvent("task", w);
    }

    auto it = m_taskStreams.find(taskId);
    if (it == m_taskStreams.end())
        return;
    const vector<JZHttpServerClient*> clients = it->second;   // 拷贝: 回调可能改动订阅表

    // 网页订阅(slim)把帧当纯信号, 内容自己 GET detail; 只有 A2A 订阅者需要完整 payload。
    // 一条 tool_result 可到 96KB, 按 150ms 一帧推下去就是 MB/s 量级的无效下行。
    bool needFull = false, needSlim = false;
    for (JZHttpServerClient *c : clients)
    {
        if (m_slimStreams.count(c))
            needSlim = true;
        else
            needFull = true;
    }
    const string full = needFull ? sseChunk(makeSseFrame(event, payload)) : string();
    const string slim = needSlim ? sseChunk(makeSseFrame(event, "{}")) : string();
    for (JZHttpServerClient *c : clients)
    {
        const string &chunk = m_slimStreams.count(c) ? slim : full;
        c->write(chunk.data(), (int)chunk.size(), false);
    }
}

// ---- 网页常连(下行) ----
void JZAgentCliServer::openPageStream(JZHttpServerClient *client)
{
    JZHttpRespone resp;
    resp.setStatusCode(200);
    resp.setContentType("text/event-stream");
    resp.setHeader("Cache-Control", "no-cache");
    resp.setHeader("Connection", "keep-alive");
    resp.setHeader("X-Accel-Buffering", "no");       // 让 nginx 别缓冲
    resp.setHeader("Transfer-Encoding", "chunked");
    client->write(resp.toHeadByteArray(0, false), false);

    m_pageStreams.push_back(client);
    connect(static_cast<JZSignal<JZObject*>*>(&client->sigDestroyed), this,
            [this, client](JZObject *) { removePageStream(client); });

    // 握手帧: 连上先给一份当前快照(重连后不必等下一次变化)
    Json::Value hello(Json::objectValue);
    hello["kind"] = "hello";
    hello["at"] = (Json::Int64)JZDateTime::currentMSecsSinceEpoch();
    hello["agents"] = onlineAgentsJson();
    const string chunk = sseChunk(makeSseFrame("hello", toJsonLine(hello)));
    client->write(chunk.data(), (int)chunk.size(), false);
}

void JZAgentCliServer::onApiEvents(JZHttpServerClient *client, const JZHttpRequest &)
{
    openPageStream(client);
}

void JZAgentCliServer::removePageStream(JZHttpServerClient *client)
{
    m_pageStreams.erase(std::remove(m_pageStreams.begin(), m_pageStreams.end(), client),
                        m_pageStreams.end());
}

void JZAgentCliServer::broadcastPageEvent(const string &name, const Json::Value &v)
{
    if (m_pageStreams.empty())
        return;
    const string chunk = sseChunk(makeSseFrame(name, toJsonLine(v)));
    const vector<JZHttpServerClient*> clients = m_pageStreams;   // 拷贝: 写失败可能回收
    for (JZHttpServerClient *c : clients)
        c->write(chunk.data(), (int)chunk.size(), false);
}

void JZAgentCliServer::broadcastAgentEvent()
{
    if (m_pageStreams.empty())
        return;
    Json::Value v(Json::objectValue);
    v["kind"] = "agents";
    v["agents"] = onlineAgentsJson();
    broadcastPageEvent("agents", v);
}

// 记录被删了: 正在跟这条任务的网页收到一帧 deleted 后关流, 免得它们等一个永远不来的终态
void JZAgentCliServer::dropTaskStreams(const string &taskId)
{
    // 常连那边也要知道这条没了(前端据此从列表移除)
    {
        Json::Value d(Json::objectValue);
        d["taskId"] = taskId;
        broadcastPageEvent("deleted", d);
    }
    auto it = m_taskStreams.find(taskId);
    if (it == m_taskStreams.end())
        return;
    Json::Value d(Json::objectValue);
    d["id"] = taskId;
    const string chunk = sseChunk(makeSseFrame("deleted", toJsonLine(d)));
    const vector<JZHttpServerClient*> clients = it->second;
    for (JZHttpServerClient *c : clients)
    {
        c->write(chunk.data(), (int)chunk.size(), false);
        c->write("0\r\n\r\n", 5, true);
        m_slimStreams.erase(c);
    }
    m_taskStreams.erase(it);
}

void JZAgentCliServer::finishTaskStreams(const string &taskId)
{
    auto it = m_taskStreams.find(taskId);
    if (it == m_taskStreams.end())
        return;
    TaskRecord *r = findTask(taskId);
    const vector<JZHttpServerClient*> clients = it->second;

    // 只在真有非 slim 订阅者时才序列化完整 Task(可能几 MB); 全是网页订阅时省掉这次大拷贝。
    bool needFull = false, needSlim = false;
    for (JZHttpServerClient *c : clients)
    {
        if (m_slimStreams.count(c))
            needSlim = true;
        else
            needFull = true;
    }
    const string full = needFull ? (r ? toJsonLine(taskJson(*r)) : string("{}")) : string();
    const string slim = needSlim
        ? (r ? toJsonLine(makeSlimTaskSignal(taskId, r->state, (Json::Int64)r->eventSeq))
             : string("{}"))
        : string();

    for (JZHttpServerClient *c : clients)
    {
        const bool isSlim = m_slimStreams.count(c) != 0;
        const string chunk = sseChunk(makeSseFrame("task", isSlim ? slim : full));
        c->write(chunk.data(), (int)chunk.size(), false);
        c->write("0\r\n\r\n", 5, true);   // 终态关流; 前端 EventSource 也要主动 close
        if (isSlim)
            m_slimStreams.erase(c);
    }
    m_taskStreams.erase(it);
}

JZAgentCliServer::TaskRecord *JZAgentCliServer::findTask(const string &taskId)
{
    auto it = m_tasks.find(taskId);
    return (it == m_tasks.end()) ? nullptr : &it->second;
}

// 本次提交的 token: task_end.usage + 最后一条 user_msg 之后的 context_compress。
// 事件列表可能被分页截断，网页不一定拿得到那条 user_msg，所以在 server 侧算好。
Json::Value JZAgentCliServer::taskUsageTurn(const TaskRecord &r) const
{
    if (!r.usage.isObject())
        return Json::Value();

    static const char *kKeys[] = {
        "prompt_tokens", "completion_tokens", "total_tokens", "cached_tokens"
    };
    Json::Value out(Json::objectValue);
    for (const char *k : kKeys)
        out[k] = (Json::Int64)0;

    auto addUsage = [&out](const Json::Value &u)
    {
        if (!u.isObject())
            return;
        for (const char *k : kKeys)
            out[k] = out[k].asInt64() + u.get(k, 0).asInt64();
    };

    addUsage(r.usage);

    // 最后一条 user_msg = 本次提交起点；老记录没有 user_msg 时保持"累加全部压缩事件"。
    bool hasUser = false;
    size_t lastUser = 0;
    for (size_t i = 0; i < r.events.size(); ++i)
    {
        Json::Value e;
        if (!parseJsonText(r.events[i], e) || !e.isObject())
            continue;
        if (e.get("type", "").asString() == "user_msg")
        {
            hasUser = true;
            lastUser = i;
        }
    }

    const size_t begin = hasUser ? lastUser + 1 : 0;
    for (size_t i = begin; i < r.events.size(); ++i)
    {
        Json::Value e;
        if (!parseJsonText(r.events[i], e) || !e.isObject())
            continue;
        if (e.get("type", "").asString() != "context_compress")
            continue;
        Json::Value d = e["data"];
        if (d.isString())
        {
            Json::Value parsed;
            if (parseJsonText(d.asString(), parsed))
                d = parsed;
        }
        addUsage(d);
    }
    return out;
}

// 对外只有这一种形状: A2A v1.0 Task。管理所需的额外信息一律放 metadata。
Json::Value JZAgentCliServer::taskJson(const TaskRecord &r, int evLimit) const
{
    ensureTaskDetail(const_cast<TaskRecord &>(r));
    Json::Value j(Json::objectValue);
    j["id"] = r.taskId;
    j["contextId"] = r.contextId;

    Json::Value st(Json::objectValue);
    st["state"] = r.state;
    if (!r.error.empty())
    {
        Json::Value msg(Json::objectValue);
        msg["messageId"] = r.taskId + ":status";
        msg["role"] = "ROLE_AGENT";
        Json::Value parts(Json::arrayValue);
        Json::Value part(Json::objectValue);
        part["text"] = r.error;
        parts.append(part);
        msg["parts"] = parts;
        st["message"] = msg;
    }
    st["timestamp"] = isoTime(r.statusMs ? r.statusMs : (r.endMs ? r.endMs : r.startMs));
    j["status"] = st;
    j["artifacts"] = jsonStringArray(r.artifacts);
    j["history"] = jsonStringArray(r.history);

    Json::Value meta(Json::objectValue);
    meta["agentId"] = r.agentId;
    meta["project"] = r.project;
    meta["prompt"] = r.prompt;
    meta["verifyCmd"] = r.verifyCmd;
    meta["origin"] = r.origin.empty() ? string(kOriginUser) : r.origin;
    meta["parentTaskId"] = r.parentTaskId;
    meta["rounds"] = r.rounds;
    meta["round"] = r.round;
    if (r.usage.isObject())
        meta["usage"] = r.usage;
    if (r.usageTurn.isObject())
        meta["usage_turn"] = r.usageTurn;
    meta["lastEventType"] = r.lastEventType;
    // 事件**全量落盘**, 但对外一次只回一页(默认最近 200 条, 起点对齐到 user_msg)。
    // 前端拿 eventsOffset > 0 就显示"加载更多", 走 /api/tasks/events 往前翻。
    // evLimit <= 0 表示全量(写 payload_json 时用)。
    const size_t evTotal = r.events.size();
    const size_t evBegin = evLimit > 0 ? eventPageBegin(r.events, evTotal, (size_t)evLimit) : 0;
    meta["events"] = jsonStringArrayFrom(r.events, evBegin, evTotal);
    meta["eventsTotal"] = (int)evTotal;
    meta["eventsOffset"] = (int)evBegin;
    meta["eventSeq"] = r.eventSeq;
    meta["startMs"] = (Json::Int64)r.startMs;
    meta["endMs"] = (Json::Int64)r.endMs;
    meta["costMs"] = (Json::Int64)(r.endMs > 0 && r.startMs > 0 ? r.endMs - r.startMs : 0);
    meta["statusMs"] = (Json::Int64)(r.statusMs ? r.statusMs : (r.endMs ? r.endMs : r.startMs));
    j["metadata"] = meta;
    return j;
}

Json::Value JZAgentCliServer::tasksJson(const string &agentId, const string &state, int limit,
                                        const string &contextId, const string &origin) const
{
    Json::Value arr(Json::arrayValue);
    // 倒序(最新在前)
    int count = 0;
    for (auto it = m_taskOrder.rbegin(); it != m_taskOrder.rend() && count < limit; ++it)
    {
        auto ti = m_tasks.find(*it);
        if (ti == m_tasks.end())
            continue;
        if (!agentId.empty() && ti->second.agentId != agentId)
            continue;
        if (!state.empty() && ti->second.state != state)
            continue;
        if (!contextId.empty() && ti->second.contextId != contextId)
            continue;
        if (!origin.empty())
        {
            const string &o = ti->second.origin.empty() ? string(kOriginUser) : ti->second.origin;
            if (o != origin)
                continue;
        }
        // 列表按 history 过滤: SQLite 启动时只载摘要, 这里按需补出 history/events。
        TaskRecord &rec = const_cast<TaskRecord &>(ti->second);
        ensureTaskDetail(rec);
        if (rec.history.empty())
            continue;
        // 列表只要"最近发生什么"给左栏/摘要用: 30 条足够, 整表体积从几百 KB 降到几十 KB
        // (选中某条时前端会再用 /api/tasks/detail 取完整一页)
        arr.append(taskJson(rec, 30));
        ++count;
    }
    return arr;
}

void JZAgentCliServer::onApiTaskList(JZHttpServerClient *client, const JZHttpRequest &req)
{
    SlowReqGuard slow("api/tasks(list)");
    const string agentId = req.queryParam("agentId");
    const string state = req.queryParam("state");
    const string contextId = req.queryParam("contextId");
    // origin 留空 = 全部(脚本/调试的既有行为); 管理台显式传 user 只看人下发的任务
    string origin = req.queryParam("origin");
    if (origin != kOriginUser && origin != kOriginAgent)
        origin.clear();
    int limit = atoi(req.queryParam("limit").c_str());
    if (limit <= 0 || limit > 500)
        limit = 50;

    Json::Value j(Json::objectValue);
    j["tasks"] = tasksJson(agentId, state, limit, contextId, origin);
    j["nextPageToken"] = "";
    j["pageSize"] = limit;
    j["totalSize"] = (int)m_tasks.size();
    respondJson(client, req, 200, j);
}

void JZAgentCliServer::onApiTaskDetail(JZHttpServerClient *client, const JZHttpRequest &req)
{
    TaskRecord *r = findTask(req.queryParam("id"));
    if (!r)
    {
        Json::Value e(Json::objectValue);
        e["error"] = "没有这个任务";
        respondJson(client, req, 404, e);
        return;
    }
    SlowReqGuard slow("api/tasks/detail");
    ensureTaskDetail(*r);
    flushOpenSegment(r);        // 正在攒的那段先落, 详情才是完整的
    respondJson(client, req, 200, taskJson(*r));
}

// 事件分页: before=<offset> 往前翻(加载更多); after=<seq> 往后增量(选中任务实时更新)。
void JZAgentCliServer::onApiTaskEvents(JZHttpServerClient *client, const JZHttpRequest &req)
{
    TaskRecord *r = findTask(req.queryParam("id"));
    if (!r)
    {
        respondJson(client, req, 404, Json::Value("没有这个任务"));
        return;
    }
    SlowReqGuard slow("api/tasks/events");
    ensureTaskDetail(*r);
    flushOpenSegment(r);

    int limit = atoi(req.queryParam("limit").c_str());
    if (limit <= 0 || limit > 2000)
        limit = 1000;
    const size_t total = r->events.size();

    // after=<seq>: 增量拉取, 只回 seq 大于该值的新事件(聊天区选中任务用)。
    const int after = atoi(req.queryParam("after").c_str());
    if (after > 0)
    {
        Json::Value arr(Json::arrayValue);
        int lastSeq = after;
        for (const string &line : r->events)
        {
            Json::Value e;
            if (!parseJsonText(line, e))
                continue;
            const int seq = e.get("seq", 0).asInt();
            if (seq <= after)
                continue;
            arr.append(e);
            lastSeq = seq;
            if (arr.size() >= (Json::ArrayIndex)limit)
                break;
        }
        Json::Value out(Json::objectValue);
        out["events"] = arr;
        out["offset"] = after;
        out["end"] = lastSeq;
        out["total"] = (int)total;
        out["hasMore"] = lastSeq < (r->eventSeq ? r->eventSeq : lastSeq);
        respondJson(client, req, 200, out);
        return;
    }

    size_t end = (size_t)atoi(req.queryParam("before").c_str());
    if (end == 0 || end > total)
        end = total;
    const size_t begin = eventPageBegin(r->events, end, (size_t)limit);

    Json::Value out(Json::objectValue);
    out["events"] = jsonStringArrayFrom(r->events, begin, end);
    out["offset"] = (int)begin;
    out["end"] = (int)end;
    out["total"] = (int)total;
    out["hasMore"] = begin > 0;
    respondJson(client, req, 200, out);
}

// A2A SubscribeToTask: 首帧必发当前 Task 快照, 之后逐帧推状态/产物事件, 终态关流。
// 传输用 chunked(与 agent_cli 的 /events 同口径): 不带 Content-Length 的流式响应在非浏览器
// 客户端里要等连接关闭才一次性交付, 那样就看不到过程了。
// ?slim=1 是网页(/tasks 详情页)专用: 它把帧当信号, 数据自己 GET detail, 首帧/终态帧只给
// 瘦信号, 免得长任务的完整 Task 单帧撑爆 send buffer。不带 slim 的 A2A 调用方行为不变。
void JZAgentCliServer::onApiTaskSubscribe(JZHttpServerClient *client, const JZHttpRequest &req)
{
    const string taskId = req.queryParam("id");
    const bool slim = req.queryParam("slim") == "1";
    if (taskId.empty())
    {
        respondJson(client, req, 400, Json::Value("缺少 id"));
        return;
    }
    TaskRecord *r = findTask(taskId);
    if (!r)
    {
        respondJson(client, req, 404, Json::Value("没有这个任务: " + taskId));
        return;
    }

    JZHttpRespone resp;
    resp.setStatusCode(200);
    resp.setContentType("text/event-stream");
    resp.setHeader("Cache-Control", "no-cache");
    resp.setHeader("Connection", "keep-alive");
    resp.setHeader("X-Accel-Buffering", "no");
    resp.setHeader("Transfer-Encoding", "chunked");
    client->write(resp.toHeadByteArray(0, false), false);

    m_taskStreams[taskId].push_back(client);
    if (slim)
        m_slimStreams.insert(client);
    connect(static_cast<JZSignal<JZObject*>*>(&client->sigDestroyed), this,
            [this, client](JZObject *) { removeTaskStream(client); });

    const string firstPayload = slim
        ? toJsonLine(makeSlimTaskSignal(taskId, r->state, (Json::Int64)r->eventSeq))
        : toJsonLine(taskJson(*r));
    const string first = sseChunk(makeSseFrame("task", firstPayload));
    client->write(first.data(), (int)first.size(), false);

    // 订阅的那一刻已经终态: 快照给完就关, 不留悬挂连接
    if (!isWorkingState(r->state))
    {
        client->write("0\r\n\r\n", 5, true);
        removeTaskStream(client);
    }
}

// A2A AgentCard: 管理台首页/详情页回答"这台 agent 是什么、能干什么"。
// 与 hello 上报是同一份数据, 只是换成标准形状; 在线状态由 server 的注册表补全。
Json::Value JZAgentCliServer::agentCardJson(const string &id) const
{
    Json::Value card(Json::objectValue);
    string name = id, version = "?", rootDir, modelName, description = "JZAgent 远端执行体";
    bool online = false, builtin = false, busy = false;
    long long firstSeen = 0, lastSeen = 0;
    int connects = 0, round = 0;
    string taskId, nodesText;

    auto ki = m_known.find(id);
    if (ki != m_known.end())
    {
        const KnownAgent &a = ki->second;
        if (!a.name.empty()) name = a.name;
        version = a.version;
        rootDir = a.rootDir;
        modelName = a.modelName;
        firstSeen = a.firstSeen;
        lastSeen = a.lastSeen;
        connects = a.connects;
    }
    auto ci = m_connByAgent.find(id);
    if (ci != m_connByAgent.end())
    {
        online = true;
        auto oi = m_onlineByConn.find(ci->second);
        if (oi != m_onlineByConn.end())
        {
            const JZCliServerOnlineAgent &o = oi->second;
            busy = o.busy;
            taskId = o.taskId;
            round = o.round;
            nodesText = o.nodes;
            lastSeen = o.lastSeen;
            if (!o.modelName.empty()) modelName = o.modelName;
            if (!o.rootDir.empty()) rootDir = o.rootDir;
        }
    }
    card["name"] = name;
    card["description"] = description;
    card["version"] = version.empty() ? string("?") : version;
    Json::Value iface(Json::objectValue);
    iface["url"] = "jznet://hub/" + id;      // 我们走自定义 JZNet 绑定, 不是 HTTP+JSON
    iface["protocolBinding"] = "JZNET_CUSTOM";
    iface["protocolVersion"] = "1.0";
    Json::Value ifaces(Json::arrayValue);
    ifaces.append(iface);
    card["supportedInterfaces"] = ifaces;
    card["defaultInputModes"] = Json::Value(Json::arrayValue);
    card["defaultInputModes"].append("text/plain");
    card["defaultOutputModes"] = Json::Value(Json::arrayValue);
    card["defaultOutputModes"].append("text/plain");

    card["capabilities"]["streaming"] = true;       // /api/tasks/subscribe
    card["capabilities"]["pushNotifications"] = false;
    card["capabilities"]["extendedAgentCard"] = false;

    Json::Value skills(Json::arrayValue);
    Json::Value s(Json::objectValue);
    s["id"] = "task";
    s["name"] = "执行任务";
    s["description"] = "接受一条自然语言指令, 在本机工作区里用工具执行并回报结果";
    s["tags"] = Json::Value(Json::arrayValue);
    s["tags"].append("task");
    s["tags"].append(builtin ? "builtin" : "remote");
    skills.append(s);

    Json::Value nodes(Json::arrayValue);
    parseJsonText(nodesText, nodes);
    if (nodes.isArray())
    {
        for (Json::ArrayIndex i = 0; i < nodes.size(); ++i)
        {
            Json::Value n(Json::objectValue);
            n["id"] = "node:" + nodes[i].get("name", "").asString();
            n["name"] = nodes[i].get("name", "node").asString();
            n["description"] = "远端执行节点: " + nodes[i].get("target", "").asString();
            n["tags"] = Json::Value(Json::arrayValue);
            n["tags"].append("node");
            skills.append(n);
        }
    }
    card["skills"] = skills;
    // 不声明 securitySchemes: 客户端证书(mTLS)已移除, 应用层 token 鉴权还没接上。

    Json::Value meta(Json::objectValue);
    meta["agentId"] = id;
    meta["online"] = online;
    meta["builtin"] = builtin;
    meta["busy"] = busy;
    meta["taskId"] = taskId;
    meta["round"] = round;
    meta["rootDir"] = rootDir;
    meta["modelName"] = modelName;
    meta["firstSeen"] = (Json::Int64)firstSeen;
    meta["lastSeen"] = (Json::Int64)lastSeen;
    meta["connects"] = connects;
    meta["nodes"] = nodes.isArray() ? nodes : Json::Value(Json::arrayValue);
    card["metadata"] = meta;
    return card;
}

void JZAgentCliServer::onApiAgentCard(JZHttpServerClient *client, const JZHttpRequest &req)
{
    const string id = req.queryParam("id");
    if (id.empty())
    {
        respondJson(client, req, 400, Json::Value("缺少 id"));
        return;
    }
    const bool exists = m_known.find(id) != m_known.end() ||
                        m_connByAgent.find(id) != m_connByAgent.end() ||
                        m_agents.find(id) != m_agents.end();
    if (!exists)
    {
        respondJson(client, req, 404, Json::Value("没有这个 agent: " + id));
        return;
    }
    respondJson(client, req, 200, agentCardJson(id));
}

// 目录浏览: GET /api/fs?path=C:/xxx
// 给网页上的"切换目录"用。只列 server 本机的目录 —— 托管模式下 cli 都是本机子进程,
// 文件系统和 server 是同一个, 所以这里看到的就是它们的工作区。
void JZAgentCliServer::onApiFs(JZHttpServerClient *client, const JZHttpRequest &req)
{
    SlowReqGuard slow("api/fs");

    const string agentId = req.queryParam("agentId");
    const string path = req.queryParam("path");

    // 目标 agent 在别的机器上 => 让那台 cli 自己列(控制面转发)
    {
        Json::Value body(Json::objectValue);
        body["path"] = path;
        body["defaultDir"] = string();
        if (fsRoute(client, req, agentId, "list", body, "json") != 0)
            return;
    }

    // 本机(没指定 agentId / agent 已离线): 直接读盘
    const string def;
    string error;
    Json::Value j = JZFileOps::list(path, def, error);
    if (j.isNull())
    {
        respondJson(client, req, 404, Json::Value(error));
        return;
    }
    respondJson(client, req, 200, j);
}

void JZAgentCliServer::onApiFsRaw(JZHttpServerClient *client, const JZHttpRequest &req)
{
    SlowReqGuard slow("api/fs/raw");

    const string agentId = req.queryParam("agentId");
    const string path = req.queryParam("path");

    {
        Json::Value body(Json::objectValue);
        body["path"] = path;
        body["maxBytes"] = (Json::Int64)(160 * 1024);   // 控制面单包 10MB, 这里按网页能用的量留余量
        if (fsRoute(client, req, agentId, "read", body, "raw") != 0)
            return;
    }

    static const int kMax = 1024 * 1024;      // 本机: 最多给前端 1MB
    string data, error;
    int64_t size = 0, mtime = 0;
    bool truncated = false;
    if (!JZFileOps::readFile(path, kMax, data, size, mtime, truncated, error))
    {
        respondJson(client, req, 404, Json::Value(error));
        return;
    }

    JZHttpRespone resp;
    resp.setStatusCode(200);
    resp.setContentType("application/octet-stream");
    resp.setHeader("Cache-Control", "no-store");
    resp.setHeader("X-File-Size", to_string(size));
    resp.setHeader("X-File-Mtime", to_string(mtime));
    if (truncated)
        resp.setHeader("X-File-Truncated", "1");
    resp.setBody(data);
    client->response(resp, req);
}

// 新建: POST /api/fs/create {agentId?, root, dir, name, kind}
//
// 边界 = **工作区内**(与 cli 的区域模型一致: 全盘可读, 工作区可写, 授信目录只读)。
// root 由前端带: 网页上切工作区只写浏览器的 localStorage, server 并不知道"这个人
// 现在看的是哪个目录"。root 只用来做前缀判定, 不参与别的逻辑。
// agentId 有值时转发给那台 cli 在本机执行(文件在它那台机器上); 否则 server 自己执行。
void JZAgentCliServer::onApiFsCreate(JZHttpServerClient *client, const JZHttpRequest &req)
{
    SlowReqGuard slow("api/fs/create");

    Json::Value body;
    if (!parseJsonText(string(req.body().data(), req.body().size()), body) || !body.isObject())
    {
        respondJson(client, req, 400, Json::Value("body 必须是 JSON 对象"));
        return;
    }

    const string agentId = body.get("agentId", "").asString();
    if (fsRoute(client, req, agentId, "create", body, "json") != 0)
        return;

    const bool isDir = body.get("kind", "dir").asString() != "file";
    string outPath, error;
    if (!JZFileOps::create(body.get("root", "").asString(), body.get("dir", "").asString(),
                           body.get("name", "").asString(), isDir, outPath, error))
    {
        respondJson(client, req, 400, Json::Value(error));
        return;
    }
    Json::Value out(Json::objectValue);
    out["ok"] = true;
    out["path"] = outPath;
    out["kind"] = isDir ? "dir" : "file";
    respondJson(client, req, 200, out);
}

void JZAgentCliServer::onApiFsRename(JZHttpServerClient *client, const JZHttpRequest &req)
{
    SlowReqGuard slow("api/fs/rename");

    Json::Value body;
    if (!parseJsonText(string(req.body().data(), req.body().size()), body) || !body.isObject())
    {
        respondJson(client, req, 400, Json::Value("body 必须是 JSON 对象"));
        return;
    }

    const string agentId = body.get("agentId", "").asString();
    if (fsRoute(client, req, agentId, "rename", body, "json") != 0)
        return;

    string outPath, error;
    if (!JZFileOps::renamePath(body.get("root", "").asString(), body.get("path", "").asString(),
                               body.get("name", "").asString(), outPath, error))
    {
        respondJson(client, req, 400, Json::Value(error));
        return;
    }
    Json::Value out(Json::objectValue);
    out["ok"] = true;
    out["path"] = outPath;
    respondJson(client, req, 200, out);
}

void JZAgentCliServer::onApiFsDelete(JZHttpServerClient *client, const JZHttpRequest &req)
{
    SlowReqGuard slow("api/fs/delete");

    Json::Value body;
    if (!parseJsonText(string(req.body().data(), req.body().size()), body) || !body.isObject())
    {
        respondJson(client, req, 400, Json::Value("body 必须是 JSON 对象"));
        return;
    }

    const string agentId = body.get("agentId", "").asString();
    if (fsRoute(client, req, agentId, "delete", body, "json") != 0)
        return;

    string error;
    if (!JZFileOps::remove(body.get("root", "").asString(), body.get("path", "").asString(), error))
    {
        respondJson(client, req, 400, Json::Value(error));
        return;
    }
    Json::Value out(Json::objectValue);
    out["ok"] = true;
    respondJson(client, req, 200, out);
}

void JZAgentCliServer::onApiProjects(JZHttpServerClient *client, const JZHttpRequest &req)
{
    SlowReqGuard slow("api/projects");
    struct Agg
    {
        std::string rootDir;
        bool online = false;
        std::map<std::string, std::pair<long long, int>> dirs;   // path -> (最近使用, 次数)
    };
    std::map<std::string, Agg> aggs;

    for (const auto &kv : m_onlineByConn)
    {
        const JZCliServerOnlineAgent &a = kv.second;
        Agg &g = aggs[a.agentId];
        g.online = true;
        g.rootDir = a.rootDir.empty() ? a.project : a.rootDir;
    }
    for (const auto &kv : m_tasks)
    {
        const TaskRecord &r = kv.second;
        if (r.project.empty())
            continue;
        Agg &g = aggs[r.agentId];
        auto it = g.dirs.find(r.project);
        if (it == g.dirs.end())
            g.dirs[r.project] = std::make_pair(r.startMs, 1);
        else
        {
            it->second.first = std::max(it->second.first, r.startMs);
            ++it->second.second;
        }
    }

    Json::Value arr(Json::arrayValue);
    for (const auto &kv : aggs)
    {
        Json::Value j(Json::objectValue);
        j["agentId"] = kv.first;
        j["online"] = kv.second.online;
        j["rootDir"] = kv.second.rootDir;
        Json::Value ps(Json::arrayValue);
        for (const auto &d : kv.second.dirs)
        {
            Json::Value p(Json::objectValue);
            p["path"] = d.first;
            p["lastMs"] = (Json::Int64)d.second.first;
            p["count"] = d.second.second;
            ps.append(p);
        }
        j["projects"] = ps;
        arr.append(j);
    }
    respondJson(client, req, 200, arr);
}

Json::Value JZAgentCliServer::snapshotTask(const string &taskId) const
{
    auto it = m_tasks.find(taskId);
    if (it == m_tasks.end())
        return Json::Value(Json::nullValue);
    return taskJson(it->second);
}

// 单个 agent 的详情: 内置 agent / 在线 cli / 本机子进程, 三选一
void JZAgentCliServer::onApiAgentDetail(JZHttpServerClient *client, const JZHttpRequest &req)
{

    const string id = req.queryParam("id");
    if (id.empty())
    {
        respondJson(client, req, 400, Json::Value("缺少 id"));
        return;
    }

    Json::Value out(Json::objectValue);
    Json::Value all = onlineAgentsJson();
    for (Json::ArrayIndex i = 0; i < all.size(); ++i)
    {
        if (all[i].get("agentId", "").asString() != id)
            continue;
        out["kind"] = all[i].get("builtin", false).asBool() ? "builtin" : "hub";
        out["info"] = all[i];
        break;
    }

    // 离线但有档案: 仍然给详情(列表页允许点进一台已经掉线的 agent)
    if (!out.isMember("info"))
    {
        auto ki = m_known.find(id);
        if (ki != m_known.end())
        {
            const KnownAgent &a = ki->second;
            Json::Value j(Json::objectValue);
            j["agentId"] = a.agentId;
            j["name"] = a.name;
            j["version"] = a.version;
            j["rootDir"] = a.rootDir;
            j["modelName"] = a.modelName;
            j["online"] = false;
            j["busy"] = false;
            j["taskId"] = "";
            j["round"] = 0;
            j["lastSeen"] = (Json::Int64)a.lastSeen;
            j["firstSeen"] = (Json::Int64)a.firstSeen;
            j["connects"] = a.connects;
            j["lastEventType"] = "";
            j["nodes"] = Json::Value(Json::arrayValue);
            out["kind"] = "offline";
            out["info"] = j;
        }
    }

    if (!out.isMember("info"))
    {
        auto it = m_agents.find(id);
        if (it == m_agents.end())
        {
            Json::Value e(Json::objectValue);
            e["error"] = "没有这个 agent: " + id + "(既不在线, 也不是本地子进程)";
            Json::Value ids(Json::arrayValue);
            for (Json::ArrayIndex i = 0; i < all.size(); ++i)
                ids.append(all[i].get("agentId", ""));
            for (const auto &kv : m_agents)
                ids.append(kv.first);
            e["known"] = ids;
            respondJson(client, req, 404, e);
            return;
        }
        JZCliServerAgentInstance *inst = it->second;
        Json::Value j(Json::objectValue);
        j["agentId"] = id;
        j["name"] = id;
        j["kind"] = "inproc";
        j["state"] = (int)inst->state;
        j["startTime"] = (Json::Int64)inst->startTime;
        j["nodes"] = Json::Value(Json::arrayValue);
        out["kind"] = "inproc";
        out["info"] = j;
    }

    out["tasks"] = tasksJson(id, "", 20, "", kOriginUser);   // 台账只列人下发的任务
    out["card"] = agentCardJson(id);      // A2A: 能力自述, 详情页直接渲染

    // 记住"最后打开的是哪台 agent": 下次访问 / 时由 onDashboard 直接 302 回来,
    // 不用等 JS 先拉一遍 projects 再猜(首屏也不会闪一下别的 agent)。
    // 只影响"打开哪页", 不是身份凭据 —— 鉴权还没做(token 待接入)。
    JZHttpCookie last("jz_agent", id);
    last.path = "/";
    last.httpOnly = true;
    last.sameSite = "Lax";
    last.secure = isClientHttps(req, m_config.http.sslEnabled);
    last.maxAge = 30 * 24 * 3600;         // 30 天
    respondJson(client, req, 200, out, &last);
}

// 这个 id 还值得打开吗: 在线的、内置的、见过的(离线档案)、本地子进程都算
bool JZAgentCliServer::agentKnown(const string &id) const
{
    if (id.empty())
        return false;
    for (auto it = m_onlineByConn.begin(); it != m_onlineByConn.end(); ++it)
    {
        if (it->second.agentId == id)
            return true;
    }
    if (m_known.find(id) != m_known.end())
        return true;
    return m_agents.find(id) != m_agents.end();
}

// 删除一台 agent 的档案(只删"曾经连过"的记录; 默认保留历史任务)
void JZAgentCliServer::onApiAgentForget(JZHttpServerClient *client, const JZHttpRequest &req)
{
    // 删除档案(还可能带 withTasks=1 连任务记录一起删): 不可逆, 必须 POST
    if (!requirePost(client, req))
        return;

    string id = req.queryParam("agentId");
    if (id.empty())
        id = req.queryParam("id");
    if (id.empty())
    {
        respondJson(client, req, 400, Json::Value("缺少 agentId"));
        return;
    }
    if (m_connByAgent.find(id) != m_connByAgent.end())
    {
        // 在线就删会导致"列表没了但连接还在", 下一次心跳又冒出来
        Json::Value e(Json::objectValue);
        e["error"] = "这台 agent 还在线, 先停掉它(或等它掉线)再删";
        e["online"] = true;
        respondJson(client, req, 409, e);
        return;
    }

    auto it = m_known.find(id);
    if (it == m_known.end())
    {
        Json::Value e(Json::objectValue);
        e["error"] = "列表里没有这台 agent";
        respondJson(client, req, 404, e);
        return;
    }
    m_known.erase(it);
    saveAgentStore();

    int removedTasks = 0;
    if (req.queryParam("withTasks") == "1")
    {
        for (auto tit = m_tasks.begin(); tit != m_tasks.end(); )
        {
            if (tit->second.agentId == id)
            {
                tit = m_tasks.erase(tit);
                ++removedTasks;
            }
            else
            {
                ++tit;
            }
        }
        vector<string> keep;
        keep.reserve(m_taskOrder.size());
        for (const auto &tid : m_taskOrder)
            if (m_tasks.find(tid) != m_tasks.end())
                keep.push_back(tid);
        m_taskOrder.swap(keep);
        rewriteTaskStore();   // 追加式文件不重写的话, 重启会把删掉的行"复活"
    }

    Json::Value j(Json::objectValue);
    j["ok"] = true;
    j["agentId"] = id;
    j["removedTasks"] = removedTasks;
    j["note"] = removedTasks ? "已删除 agent 及其任务记录"
                             : "只从列表移除; 任务记录保留(加 withTasks=1 可一并删除)";
    jzInfo().noquote().nospace() << "agent 档案删除: " << id << "  tasks=" << removedTasks;
    respondJson(client, req, 200, j);
}

void JZAgentCliServer::servePage(JZHttpServerClient *client, const JZHttpRequest &req,
                                 const std::string &name)
{
    // 桌面入口 index.html 在手机浏览器上改投 mobile.html; 其它页面(如 tasks)照旧。
    const string file = (name == "index.html" && isMobileBrowser(req)) ? "mobile.html" : name;
    const string path = dashboardDir() + "/" + file;
    ifstream ifs(path, ios::binary);
    if (!ifs)
    {
        respondText(client, req, 404,
                    "页面不存在: " + path + " (发布时要带上 server_web/)", "text/plain; charset=utf-8");
        return;
    }
    stringstream ss;
    ss << ifs.rdbuf();
    // 页面本身不带版本号(只有 app.js/app.css 有 ?v=), 所以必须每次校验:
    // 否则浏览器留着旧 HTML, 里面的 ?v= 也是旧的, 改完前端刷新看不到效果。
    respondText(client, req, 200, ss.str(), "text/html; charset=utf-8", "no-cache", "User-Agent");
}

// ---------------------------------------------------------------------------
// 任务下发: 核心(HTTP 与 server 内置 agent 的工具共用)
//   返回 200 = 已登记同步等待(调用方**不要**再回包, 回包在 taskEnd/done 时发出)
//   返回 202 = 已接受(异步)
//   返回 >=400 = 失败(ack 里有 error, 调用方直接回包)
// ---------------------------------------------------------------------------
int JZAgentCliServer::submitTask(const Json::Value &body, JZHttpServerClient *waiter,
                                 Json::Value &ack, string &error,
                                 const string &origin, const string &parentTaskId,
                                 const string &acceptEncoding)
{
    // A2A SendMessage 是唯一入口。cli 的 /task 载荷在边界转换一次, 对外不再出现 prompt/verify_cmd。
    const string prompt = messageText(body);
    // 图片附件(多模态): 网页传来的 data URL 数组, 原样透传给 cli(它的 extractImages 认这个字段)。
    // 允许"只发图不写字" —— 与 cli 侧同一口径。
    const Json::Value &images = body["images"];
    const bool hasImages = images.isArray() ? images.size() > 0 : images.isString();
    if (prompt.empty() && !hasImages)
    {
        error = "缺少 message.parts[].text（只发图片时请用 images 字段）";
        ack["error"] = error;
        return 400;
    }
    string agentId = body.get("agentId", "").asString();
    if (agentId.empty() && m_onlineByConn.size() == 1)
        agentId = m_onlineByConn.begin()->second.agentId;   // 只有一台就默认它
    if (agentId.empty())
    {
        error = "缺少 agentId(当前在线 " + to_string(m_onlineByConn.size()) + " 台)";
        ack["error"] = error;
        return 400;
    }

    // 网页「模型」选择: 只对这一条任务生效(cli/内置 agent 都只认各自配置里已有的名字)
    string wantModel;
    if (body.isMember("modelName") && body["modelName"].isString())
        wantModel = body["modelName"].asString();

    Json::Value cliBody(Json::objectValue);
    cliBody["prompt"] = prompt;
    // ⚠ 以前这里只搬了 prompt/verify_cmd 等字段, images 被**静默丢掉** —— 网页传了图、cli 也支持,
    //   但中间这一层没转, 模型只看到文字(2026-11-12 实测: 模型跑去 workspace 里找文件猜内容)。
    if (!images.isNull())
        cliBody["images"] = images;
    cliBody["verify_cmd"] = body.get("verifyCmd", "").asString();
    if (!wantModel.empty())
        cliBody["modelName"] = wantModel;
    cliBody["use_memory"] = false;
    if (body.isMember("verifyTimeout"))   cliBody["verify_timeout"] = body["verifyTimeout"];
    if (body.isMember("maxVerifyRounds")) cliBody["max_verify_rounds"] = body["maxVerifyRounds"];
    if (body.isMember("maxRetries"))      cliBody["max_retries"] = body["maxRetries"];
    if (body.isMember("maxRounds"))       cliBody["max_rounds"] = body["maxRounds"];
    // 网页上切换目录: 任务自带工作目录, cli 按它选/建任务对象(不改进程配置)
    const string wantRoot = body.get("rootDir", "").asString();
    if (!wantRoot.empty())
        cliBody["rootDir"] = wantRoot;

    // 「接着说」: 请求里带 taskId 就复用那条任务 —— 同一个任务号接着写, 前端聊天区天然连续。
    // 不带(或查不到)就照旧新建一条。
    TaskRecord *contRec = nullptr;
    const string contId = body.get("taskId", "").asString();
    if (!contId.empty())
    {
        contRec = findTask(contId);
        if (!contRec)
        {
            error = "要接续的任务不存在: " + contId;
            ack["error"] = error;
            return 404;
        }
        if (contRec->agentId != agentId)
        {
            error = "任务 " + contId + " 属于 agent " + contRec->agentId + ", 不能接着它发给 " + agentId;
            ack["error"] = error;
            return 400;
        }
        if (isWorkingState(contRec->state) ||
            m_pendingTasks.find(contId) != m_pendingTasks.end())
        {
            error = "任务 " + contId + " 还在执行, 不能接着说";
            ack["error"] = error;
            return 409;
        }
        // 续聊上下文以 ctx 文件为准: 没有就没法真正接着聊, 不要静默当新会话跑。
        Json::Value savedCtx = loadContextFile(contRec->contextId);
        if (!savedCtx.isObject() || !savedCtx["history"].isArray() || savedCtx["history"].empty())
        {
            error = "任务 " + contId + " 没有可继续的上下文";
            ack["error"] = error;
            return 409;
        }
    }

    const string taskId = contRec ? contRec->taskId
                                  : ("t" + to_string(JZDateTime::currentMSecsSinceEpoch()) + "-"
                                     + to_string(++m_taskSeq));

    // 「无响应」判定: 距**最后一条事件**超过 timeoutMs 才算超时, 由 5s 巡检(reapStuckTasks)
    // 用 JZPlatform::getTickCountMs() 比较, 不给每个任务单开定时器 —— 慢但一直在推事件的
    // 任务不会被误杀, 真正没动静的(LLM 不响应 / cli 失联)才会被判失败。
    int timeoutMs = body.get("taskTimeoutMs", 900000).asInt();
    if (timeoutMs < 1000) timeoutMs = 1000;
    if (timeoutMs > 24 * 3600 * 1000) timeoutMs = 24 * 3600 * 1000;

    if (waiter)
    {
        // 提交方提前断开: 只丢回包通道, 任务照跑(cli 侧行为与 HTTP /task 一致)
        connect(static_cast<JZSignal<JZObject*>*>(&waiter->sigDestroyed), this, [this](JZObject *obj)
        {
            for (auto &kv : m_pendingTasks)
                if (kv.second.client == static_cast<JZHttpServerClient *>(obj))
                    kv.second.client = nullptr;
        });
    }

    // ---- 远端 cli: 走控制面 ----
    auto ai = m_connByAgent.find(agentId);
    if (ai == m_connByAgent.end())
    {
        error = "没有在线的 agent: " + agentId;
        ack["error"] = error;
        Json::Value ids(Json::arrayValue);
        for (const auto &kv : m_connByAgent)
            ids.append(kv.first);
        ack["online"] = ids;
        return 404;
    }

    JZCliServerOnlineAgent &agent = m_onlineByConn[ai->second];
    // 注意: 这里**不再**因为 agent.busy 就 409 —— cli 侧有任务队列(task.queues),
    // 忙时它会自己排队, server 拦下来反而把"多队列"能力废掉了。

    // 先落记录: 后面任何一条退出路径都能查到它(接着说 = 往原记录里追加一轮)
    if (contRec)
        recordContinue(taskId, body);
    else
        recordCreate(taskId, agentId, body, origin, parentTaskId);

    // 续会话: 这条会话的对话快照(没有 = 新会话)。cli 端一次性任务对象, 必须显式带过去。
    // 读出来先过单包预算: 超限就在应用层裁历史, 不让 formatPacket 撞 FATAL。
    TaskRecord *recCtx = findTask(taskId);
    Json::Value submitCtx;
    if (recCtx)
        submitCtx = loadContextFile(recCtx->contextId);
    // 压缩只影响"发给模型的这一段": ctx 文件保留完整历史(聊天记录要用),
    // 取上下文时从最后一条 summary 开始截 —— 摘要在内, 它前面的都是被压缩掉的旧消息。
    trimHistoryToLastSummary(submitCtx);

    JZHubPackTaskSubmit submit;
    if (!fitTaskSubmitContext(submitCtx, cliBody, taskId, submit, error))
    {
        error = "任务 " + taskId + ": " + error;
        ack["error"] = error;
        ack["taskId"] = taskId;
        recordFail(taskId, kStateFailed, error);
        return 413;
    }
    if (submitCtx.isObject())
        jzInfo() << "续会话: " << taskId << " 带上 " << submitCtx["history"].size() << " 条历史";

    PendingTask pending;
    pending.taskId = taskId;
    pending.agentId = agentId;
    pending.client = waiter;
    pending.startMs = JZDateTime::currentMSecsSinceEpoch();
    pending.lastEventMs = JZPlatform::getTickCountMs();
    pending.timeoutMs = timeoutMs;
    pending.acceptEncoding = acceptEncoding;
    m_pendingTasks[taskId] = pending;

    agent.busy = true;
    agent.taskId = taskId;
    agent.round = 0;

    sendPack(ai->second, &submit);
    jzInfo().noquote().nospace() << "任务下发: " << taskId << " -> " << agentId;

    ack = taskJson(*findTask(taskId));          // SendMessage 的回包就是一个 Task
    return waiter ? 200 : 202;
}

int JZAgentCliServer::submitTaskForTool(const Json::Value &body, Json::Value &ack, string &error)
{
    // server_* 工具派出去的活: 记成 agent 来源, 不等回包。
    return submitTask(body, nullptr, ack, error, kOriginAgent);
}

// ---------------------------------------------------------------------------
// A2A SendMessage(POST /api/tasks): 默认阻塞到终态; returnImmediately=true 立即回 Task。
// ---------------------------------------------------------------------------
void JZAgentCliServer::onApiTasks(JZHttpServerClient *client, const JZHttpRequest &req)
{
    // 下发任务 = 让 agent 在本机跑东西, 是最重的副作用接口: 与其它动作接口同一口径, 只收 POST
    // (文档里本来就写成 POST /api/tasks; 之前只是没强制)。
    if (!requirePost(client, req))
        return;

    SlowReqGuard slow("api/tasks(submit)");

    Json::Value body;
    if (!parseJsonText(string(req.body().data(), req.body().size()), body) || !body.isObject())
    {
        respondJson(client, req, 400, Json::Value("body 必须是 JSON 对象"));
        return;
    }

    JZHttpServerClient *waiter = body.get("returnImmediately", false).asBool() ? nullptr : client;

    Json::Value ack(Json::objectValue);
    string error;
    const int code = submitTask(body, waiter, ack, error, std::string("user"),
                                std::string(), req.header("Accept-Encoding"));
    if (code == 200)
        return;   // 同步模式: 回包在 taskEnd / done 时发出
    respondJson(client, req, code, ack);
}

void JZAgentCliServer::onApiTaskCancel(JZHttpServerClient *client, const JZHttpRequest &req)
{
    if (!requirePost(client, req))
        return;

    // A2A CancelTask: 用 Task.id 指定任务(?id= 或 body {"id": "..."})
    string taskId = req.queryParam("id");
    if (taskId.empty())
    {
        Json::Value body;
        if (parseJsonText(string(req.body().data(), req.body().size()), body) && body.isObject())
            taskId = body.get("id", "").asString();
    }
    if (taskId.empty())
    {
        respondJson(client, req, 400, Json::Value("缺少 id"));
        return;
    }

    auto pit = m_pendingTasks.find(taskId);
    if (pit == m_pendingTasks.end())
    {
        Json::Value e(Json::objectValue);
        e["error"] = "没有这个在跑的任务: " + taskId;
        respondJson(client, req, 404, e);
        return;
    }

    auto ai = m_connByAgent.find(pit->second.agentId);
    if (ai != m_connByAgent.end() && m_hub)
    {
        JZHubPackTaskCancel cancel;
        cancel.taskId = taskId;
        sendPack(ai->second, &cancel);
    }

    // A2A CancelTask: 返回更新后的 Task, 且**不保证真的取消成功**(可能刚好已跑完)。
    // 状态仍是非终态是对的, 前端要继续跟到终态, 不能自己把徽标改成 canceled。
    TaskRecord *rec = findTask(taskId);
    respondJson(client, req, 202, rec ? taskJson(*rec) : Json::Value(Json::objectValue));
}

// 删除历史任务记录: 只删"已经不在跑"的, 在跑的要先取消(删了取消/收尾都找不到记录)。
// POST /api/tasks/delete
//   body {"id": "..."} 或 {"ids": [...]}; 也接受 ?id=
void JZAgentCliServer::onApiTaskDelete(JZHttpServerClient *client, const JZHttpRequest &req)
{
    if (!requirePost(client, req))
        return;

    vector<string> ids;

    const string qid = req.queryParam("id");
    if (!qid.empty())
        ids.push_back(qid);

    Json::Value body;
    if (parseJsonText(string(req.body().data(), req.body().size()), body) && body.isObject())
    {
        const Json::Value &jid = body["id"];
        if (jid.isString() && !jid.asString().empty())
            ids.push_back(jid.asString());
        const Json::Value &list = body["ids"];
        if (list.isArray())
        {
            for (Json::ArrayIndex i = 0; i < list.size(); ++i)
                if (list[i].isString())
                    ids.push_back(list[i].asString());
        }
    }
    if (ids.empty())
    {
        respondJson(client, req, 400, Json::Value("缺少 id(或 ids 数组)"));
        return;
    }

    int removed = 0, canceled = 0;
    Json::Value skipped(Json::arrayValue);
    vector<string> goneCtx;      // 删掉的记录属于哪些会话: 会话彻底没了才清 ctx 文件
    for (const auto &tid : ids)
    {
        if (tid.empty())
            continue;
        auto it = m_tasks.find(tid);
        if (it == m_tasks.end())
            continue;

        // 还在跑: 先让 agent 停手再删。删完这条记录就没了, 后续事件/收尾都会因
        // findTask() 查不到而自动丢弃(见 recordEvent/recordEnd 的空指针判断)。
        if (isWorkingState(it->second.state) ||
            m_pendingTasks.find(tid) != m_pendingTasks.end())
        {
            dropTaskStreams(tid);   // 先推 deleted 再取消: 取消可能同步收尾并关流
            const string agentId = it->second.agentId;
            auto ai = m_connByAgent.find(agentId);
            if (ai != m_connByAgent.end() && m_hub)
            {
                JZHubPackTaskCancel cancel;
                cancel.taskId = tid;
                sendPack(ai->second, &cancel);
            }
            ++canceled;
        }
        else
        {
            dropTaskStreams(tid);   // 已结束: 只是有人还挂着看
        }
        goneCtx.push_back(it->second.contextId);
        m_tasks.erase(it);
        ++removed;
    }

    if (removed > 0)
    {
        vector<string> keep;
        keep.reserve(m_taskOrder.size());
        for (const auto &tid : m_taskOrder)
            if (m_tasks.find(tid) != m_tasks.end())
                keep.push_back(tid);
        m_taskOrder.swap(keep);
        rewriteTaskStore();   // 追加式文件不重写的话, 重启会把删掉的行"复活"
        for (const auto &cid : goneCtx)
            dropContextIfUnused(cid);   // 这条会话一条任务都不剩了: 上下文文件也删掉
        jzInfo().noquote().nospace() << "任务记录删除: " << removed << " 条"
                                     << (canceled ? "(其中在跑的已请求取消)" : "");
    }

    Json::Value j(Json::objectValue);
    j["ok"] = removed > 0;
    j["removed"] = removed;
    j["canceled"] = canceled;
    j["skipped"] = skipped;
    if (removed == 0)
        j["error"] = "没有这条任务记录";
    respondJson(client, req, removed > 0 ? 200 : 404, j);
}

// ---------------------------------------------------------------------------
// 后台网页 + 管理 API
// ---------------------------------------------------------------------------
// /api/* 的鉴权总闸: 失败计数 + 临时封禁(按客户端 IP) + token 校验。
// 返回 false 表示已经回过包(401/403), 调用方直接结束本次请求。
bool JZAgentCliServer::httpAuthGuard(JZHttpServerClient *client, const JZHttpRequest &req)
{
    // 保护范围: /api/*(数据与动作) + /app.js(前端逻辑与接口约定)。
    // 不保护: 页面 HTML、css、图片、以及 boot.js —— 这些"拿到也没用", 而"输入 token"
    // 的界面本身要靠它们(boot.js 内联自足)才能加载, 护上就等于把人锁在门外看不到提示。
    const string &path = req.path();
    const bool isApi = (path.compare(0, 5, "/api/") == 0);
    if (!isApi && path != "/app.js")
        return true;

    // ⓪ CSRF: 跨站请求直接拒, 且**不进封禁计数** —— 它带的是受害者的正确 token,
    //   记成"失败"只会把受害者自己封掉。放在 token 校验之前, 因为它和"token 对不对"无关。
    if (isApi && isCrossSiteRequest(req))
    {
        jzWarning() << ("跨站请求已拒绝: " + path).c_str();
        Json::Value body(Json::objectValue);
        body["error"] = "跨站请求已拒绝(只接受本站页面的 /api 调用)";
        body["banned"] = false;
        respondJson(client, req, 403, body);
        return false;
    }

    // 客户端 IP: 按 http.trustProxy 这个部署策略算(框架只报 TCP 对端)
    const string ip = clientIpOf(req, client ? client->peerIp() : string(),
                                 m_config.http.trustProxy);
    const int64_t now = JZPlatform::getTickCountMs();

    // ① 封禁中: 直接 403, 连 token 都不比(否则爆破脚本还能继续试)
    {
        JZMutexLocker lock(&m_authMutex);
        auto it = m_authFails.find(ip);
        if (it != m_authFails.end() && it->second.banUntil > now)
        {
            const int64_t leftSec = (it->second.banUntil - now + 999) / 1000;
            Json::Value body(Json::objectValue);
            body["error"] = "token 连续错误, 已临时封禁 " + to_string((long long)leftSec) + " 秒";
            body["banned"] = true;
            body["retryAfterSec"] = (Json::Int64)leftSec;
            respondJson(client, req, 403, body);
            return false;
        }
    }

    // ② token 正确: 清掉这台 IP 的失败记录
    const string tk = tokenFromRequest(req);
    if (tokenEquals(tk, m_config.http.token))
    {
        JZMutexLocker lock(&m_authMutex);
        m_authFails.erase(ip);
        return true;
    }

    // ②.5 "压根没带 token" 不算失败: 浏览器第一次打开页面必然先打几条 /api/*,
    //     把它们算成爆破会让正常用户在输入 token 之前就被自己的刷新封掉。
    //     爆破的特征是"一直带着不同 token 来试", 那种才计数。
    if (tk.empty())
    {
        Json::Value body(Json::objectValue);
        body["error"] = "需要 token";
        body["needToken"] = true;
        respondJson(client, req, 401, body);
        return false;
    }

    // ③ 带错了 token: 记一次失败; 到阈值就开始封
    int banSeconds = 0;
    {
        JZMutexLocker lock(&m_authMutex);
        AuthFailRecord &r = m_authFails[ip];
        const int64_t windowMs = 5 * 60 * 1000;
        if (r.windowStart == 0 || now - r.windowStart > windowMs)
        {
            r.windowStart = now;
            r.fails = 0;
        }
        r.fails++;
        if (r.fails >= m_config.http.authMaxFails && m_config.http.authBanSeconds > 0)
        {
            r.banUntil = now + (int64_t)m_config.http.authBanSeconds * 1000;
            r.fails = 0;
            r.windowStart = now;
            banSeconds = m_config.http.authBanSeconds;
        }
    }

    if (banSeconds > 0)
    {
        jzWarning() << ("API token 连续错误, 临时封禁 " + ip + " " + to_string(banSeconds) + " 秒").c_str();
        Json::Value body(Json::objectValue);
        body["error"] = "token 连续错误, 已临时封禁 " + to_string(banSeconds) + " 秒";
        body["banned"] = true;
        body["retryAfterSec"] = banSeconds;
        respondJson(client, req, 403, body);
        return false;
    }

    Json::Value body(Json::objectValue);
    body["error"] = "token 不正确";
    body["needToken"] = true;
    respondJson(client, req, 401, body);
    return false;
}

// 定期丢过期记录: 否则被扫过的 IP 会一直留在 map 里(内网端口被公网扫是常态)
void JZAgentCliServer::pruneAuthFails()
{
    const int64_t now = JZPlatform::getTickCountMs();
    const int64_t windowMs = 5 * 60 * 1000;

    JZMutexLocker lock(&m_authMutex);
    for (auto it = m_authFails.begin(); it != m_authFails.end();)
    {
        const AuthFailRecord &r = it->second;
        const bool banExpired = (r.banUntil == 0 || r.banUntil <= now);
        const bool windowExpired = (r.windowStart == 0 || now - r.windowStart > windowMs);
        if (banExpired && windowExpired)
            it = m_authFails.erase(it);
        else
            ++it;
    }
}

bool JZAgentCliServer::startHttp(std::string &error)
{
    m_http = new JZHttpServer(this);

    // HTTPS: 证书在 init() 里已读进内存(读不到就不会走到这里)。
    //   只给 cert/key、不给 sslCa ⇒ 单向 TLS(仅加密, 不验客户端)。
    //   客户端证书(mTLS)已于 2026-11-14 移除, 应用层鉴权(登录/token)后续补。
    if (m_config.http.sslEnabled)
    {
        JZSSLConfig ssl;
        ssl.sslCert = m_httpCertPem;
        ssl.sslKey = m_httpKeyPem;
        m_http->setSslConfig(ssl);
    }
    else
    {
        jzWarning() << "http.ssl.enabled=false: 管理台/API 将以明文 HTTP 暴露(仅限完全可信的隔离网络)";
    }
    // 客户端 IP 的来源(应用层策略, 见 httpAuthGuard 里的 clientIpOf):
    // 默认只认 TCP 对端; 挡在 nginx 后面时由配置显式打开(read X-Forwarded-For)
    if (m_config.http.trustProxy)
        jzWarning() << "http.trustProxy=true: 客户端 IP 取自 X-Forwarded-For, "
                       "请确认端口只对反向代理可达(否则按 IP 的失败封禁可被伪造绕过)";
    // 应用层鉴权: 只护 /api/*(静态页/JS 不护, 否则连输入 token 的界面都拿不到)。
    // TLS 只保证"路上没人看", 不保证"对面是谁", 所以这两层必须同时开。
    if (!m_config.http.token.empty())
    {
        m_http->setAuthFilter([this](JZHttpServerClient *client, const JZHttpRequest &req)
        {
            return httpAuthGuard(client, req);
        });
    }

    // 任务记录 + agent 档案: 先载盘, 否则重启后历史全丢
    loadTaskStore();
    loadAgentStore();

    // SSE 订阅保活: 长任务没有事件时, 代理/浏览器可能把空闲连接掐掉
    m_streamTimer = new JZTimer(this);
    connect(&m_streamTimer->sigTimeout, this, [this]()
    {
        const string chunk = sseChunk(": keepalive\n\n");
        for (auto &kv : m_taskStreams)
            for (JZHttpServerClient *c : kv.second)
                c->write(chunk.data(), (int)chunk.size(), false);
        // 常连也要保活: 空转 15s 一个注释帧, 代理/浏览器不会把空闲连接掐掉
        for (JZHttpServerClient *c : m_pageStreams)
            c->write(chunk.data(), (int)chunk.size(), false);
        // agent 状态每 15s 兜一次快照(上线/下线本来有即时推送, 这里防丢帧)
        broadcastAgentEvent();
    });
    m_streamTimer->start(15000);

    // 管理 API(精确路由, 优先于兜底)
    m_http->addRoute("/api/status", new ApiStatusHandler(this));
    m_http->addRoute("/api/tasks/cancel", new ApiTaskCancelHandler(this));
    m_http->addRoute("/api/tasks/delete", new ApiTaskDeleteHandler(this));
    m_http->addRoute("/api/tasks/detail", new ApiTaskDetailHandler(this));
    m_http->addRoute("/api/tasks/events", new ApiTaskEventsHandler(this));
    m_http->addRoute("/api/tasks/subscribe", new ApiTaskSubscribeHandler(this));
    m_http->addRoute("/api/events", new ApiEventsHandler(this));
    m_http->addRoute("/api/tasks", new ApiTasksHandler(this));
    m_http->addRoute("/api/agent", new ApiAgentDetailHandler(this));
    m_http->addRoute("/api/agent/card", new ApiAgentCardHandler(this));
    m_http->addRoute("/api/fs", new ApiFsHandler(this));
    m_http->addRoute("/api/fs/raw", new ApiFsRawHandler(this));
    m_http->addRoute("/api/fs/create", new ApiFsCreateHandler(this));
    m_http->addRoute("/api/fs/rename", new ApiFsRenameHandler(this));
    m_http->addRoute("/api/fs/delete", new ApiFsDeleteHandler(this));
    m_http->addRoute("/api/projects", new ApiProjectsHandler(this));
    m_http->addRoute("/api/agent/forget", new ApiAgentForgetHandler(this));
    m_http->addRoute("/api/agents", new ApiAgentsHandler(this));
    m_http->addRoute("/api/agents/logs", new ApiAgentLogsHandler(this));
    m_http->addRoute("/api/agents/start", new ApiAgentActionHandler(this, "start"));
    m_http->addRoute("/api/agents/stop", new ApiAgentActionHandler(this, "stop"));
    m_http->addRoute("/api/agents/restart", new ApiAgentActionHandler(this, "restart"));

    // 管理台页面资源: 全部来自 <exeDir>/server_web/
    //   CMake 从 binary/agent_cli/webui(与 cli 自带网页同一份前端) + web/ui 拷过来
    //   /app.css /app.js   页面自己的样式与脚本
    //   /ui/*              共享视觉层(tokens/base/components/jz-ui.js)
    // 目录缺失时 `/` 会回 500 并说明"缺 server_web/", 不会静默给出空白页。
    JZHttpStaticFiles *webFiles = new JZHttpStaticFiles(dashboardDir());
    // 管理台的页面每次构建都可能变: 让浏览器回源校验(ETag), 不要吃 1 小时缓存
    webFiles->setCacheMaxAge(0);
    m_http->addRoute("/app.css", webFiles);
    m_http->addRoute("/app.js", webFiles);
    // boot.js: 唯一**不鉴权**的脚本 —— 它先探一下 token, 通过了再动态加载 app.js
    // (app.js 被保护, 而"输入 token"的界面不能写在 app.js 里)。
    m_http->addRoute("/boot.js", webFiles);
    m_http->addRoute("/mobile.css", webFiles);   // 手机页专属样式
    m_http->addRoute("/ui/<path:path>", webFiles);
    // /vendor/*  第三方前端库(highlight.js 等), 全是本地文件 —— 不走 CDN
    m_http->addRoute("/vendor/<path:path>", webFiles);

    // 页面: 桌面 index.html 是对话页, 手机 UA 由 servePage 改投 mobile.html。
    // /agent 是老 ?id= 书签的别名; /mobile.html 允许显式访问(真机调试/书签)。
    m_http->addRoute("/agent", new PageHandler(this, "index.html"));
    m_http->addRoute("/mobile.html", new PageHandler(this, "mobile.html"));
    m_http->addRoute("/tasks", new PageHandler(this, "tasks.html"));

    // 首页 + 兜底(未知路径也回首页, 方便深链); 同样按 UA 分流
    m_http->addRoute("/", new DashboardHandler(this));
    m_http->addRoute("/<path:path>", new DashboardHandler(this));

    if (!m_http->listen(m_config.http.bind, m_config.http.port))
    {
        // HTTPS 下"端口没起来"和"浏览器握手失败"在外部看起来一样, 所以这里必须说清楚是监听失败,
        // 而不是让运维去怀疑证书。
        error = string("管理网页监听失败: ") + (m_config.http.sslEnabled ? "https://" : "http://")
                + m_config.http.bind + ":" + to_string(m_config.http.port)
                + " (端口被占用, 或 bind 地址不属于本机网卡)";
        jzWarning() << error.c_str();
        return false;
    }
    jzInfo() << "管理网页已启动: " << (m_config.http.sslEnabled ? "https://" : "http://")
             << m_config.http.bind << ":" << m_config.http.port
             << (m_config.http.sslEnabled ? "  (单向 TLS, 无客户端认证)" : "  (明文!)");
    return true;
}

// ---------------------------------------------------------------------------
// agent 管理
// ---------------------------------------------------------------------------

// inproc 模式: 从 args 里取配置文件(-c/--config), 相对路径按 dir(或 exeDir)展开。
// 为什么要自己解析: inproc 的 cli 没有独立进程的 cwd(它就跑在 server 进程里),
// 直接拿相对路径会按 server 的 cwd 找, 与 process 模式的语义不一致。
static string resolveAgentConfigPath(const JZCliServerAgentConfig &cfg)
{
    string path;
    for (size_t i = 0; i < cfg.args.size(); i++)
    {
        if ((cfg.args[i] == "-c" || cfg.args[i] == "--config") && i + 1 < cfg.args.size())
        {
            path = cfg.args[i + 1];
            break;
        }
    }
    if (path.empty())
        path = "config.json";

    const bool isAbs = path[0] == '/' || path[0] == '\\' ||
                       (path.size() > 1 && path[1] == ':');
    if (isAbs)
        return path;
    const string base = cfg.dir.empty() ? JZPlatform::applicationDirPath() : cfg.dir;
    return base + "/" + path;
}

bool JZAgentCliServer::spawnAgent(const string &name)
{
    auto it = m_agents.find(name);
    if (it == m_agents.end())
        return false;
    JZCliServerAgentInstance *inst = it->second;
    if (inst->state == JZCliAgent_Running || inst->state == JZCliAgent_Starting)
        return false;   // 已在跑

    // 配置文件: args 里的 -c <path>(与独立 exe 同一套); 缺省 = <dir 或 exeDir>/config.json
    const string cfgPath = resolveAgentConfigPath(inst->cfg);

    // 每次启动用新对象: JZThread 跑过一轮就 join 掉了, 复用容易踩状态残留
    if (inst->inproc)
    {
        inst->inproc->stop();
        delete inst->inproc;
        inst->inproc = nullptr;
    }
    inst->inproc = new JZInprocCli();

    inst->state = JZCliAgent_Starting;
    string err;
    if (!inst->inproc->startCli(cfgPath, inst->cfg.name,
                                "127.0.0.1", m_config.hub.port, m_config.hub.token,
                                inst->cfg.tools, err))
    {
        inst->state = JZCliAgent_Stopped;
        appendLog(inst, "[error] 启动失败: " + err);
        jzWarning() << "agent 启动失败:" << name.c_str() << err.c_str();
        return false;
    }

    // 线程起来 = Running(它自己会去连控制面, 连上后才会出现在 hub 的在线列表里)
    inst->state = JZCliAgent_Running;
    inst->startTime = JZDateTime::currentMSecsSinceEpoch();
    inst->stopTime = 0;
    jzInfo() << "agent 启动:" << name.c_str() << "  cfg=" << cfgPath.c_str();
    return true;
}

bool JZAgentCliServer::stopAgent(const string &name)
{
    auto it = m_agents.find(name);
    if (it == m_agents.end())
        return false;
    JZCliServerAgentInstance *inst = it->second;
    if (inst->state != JZCliAgent_Running && inst->state != JZCliAgent_Starting)
        return false;

    // 退出 cli 的事件循环并 join: 内部会析构 JZAgentCli, 控制面连接随之断开
    if (inst->inproc)
        inst->inproc->stop();
    {
        JZMutexLocker lock(&inst->mutex);
        inst->state = JZCliAgent_Stopped;
        inst->stopTime = JZDateTime::currentMSecsSinceEpoch();
    }
    jzInfo() << "agent 停止:" << name;
    return true;
}

bool JZAgentCliServer::restartAgent(const string &name)
{
    auto it = m_agents.find(name);
    if (it == m_agents.end())
        return false;
    JZCliServerAgentInstance *inst = it->second;
    if (inst->state == JZCliAgent_Running || inst->state == JZCliAgent_Starting)
    {
        if (inst->inproc)
            inst->inproc->stop();
    }
    {
        JZMutexLocker lock(&inst->mutex);
        inst->state = JZCliAgent_Stopped;
    }
    return spawnAgent(name);
}

// ---------------------------------------------------------------------------
// 日志收集(读线程调用): 拆行入环形缓冲
// ---------------------------------------------------------------------------
void JZAgentCliServer::appendLog(JZCliServerAgentInstance *inst, const string &chunk)
{
    // 只用于启停阶段的少量记录(运行日志走 JZLog 的 Agent 模块 -> Agent.log)
    size_t pos = 0;
    while (pos <= chunk.size())
    {
        size_t nl = chunk.find('\n', pos);
        string line = (nl == string::npos) ? chunk.substr(pos) : chunk.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!line.empty())
        {
            JZMutexLocker lock(&inst->mutex);
            inst->logs.push_back(line);
            while (inst->logs.size() > inst->logMax)
                inst->logs.pop_front();
        }
        if (nl == string::npos)
            break;
        pos = nl + 1;
    }
}

// ---------------------------------------------------------------------------
// 路由实现
// ---------------------------------------------------------------------------
void JZAgentCliServer::onApiStatus(JZHttpServerClient *client, const JZHttpRequest &req)
{
    Json::Value j;
    j["http"]["bind"] = m_config.http.bind;
    j["http"]["port"] = m_config.http.port;
    j["hub"]["bind"] = m_config.hub.bind;
    j["hub"]["port"] = m_config.hub.port;
    j["hub"]["listening"] = (m_hub != nullptr && m_hub->isListening());
    j["uptimeMs"] = (int)(JZDateTime::currentMSecsSinceEpoch() - m_startTime);
    Json::Value arr = Json::arrayValue;
    for (auto &kv : m_agents)
        arr.append(agentToJson(kv.second));
    j["agents"] = arr;
    // 控制面已上线的 cli(托管模式): 与"本机子进程"是两个来源, 分开列
    j["online"] = onlineAgentsJson();
    // 曾经连过的全部(含离线): 列表页要能回答"这台以前在不在"
    j["known"] = knownAgentsJson();
    respondJson(client, req, 200, j);
}

void JZAgentCliServer::onApiAgents(JZHttpServerClient *client, const JZHttpRequest &req)
{
    Json::Value j;
    Json::Value arr = Json::arrayValue;
    for (auto &kv : m_agents)
        arr.append(agentToJson(kv.second));
    j["agents"] = arr;
    j["online"] = onlineAgentsJson();
    respondJson(client, req, 200, j);
}

void JZAgentCliServer::onApiAgentLogs(JZHttpServerClient *client, const JZHttpRequest &req)
{
    string name = req.queryParam("name");
    auto it = m_agents.find(name);
    if (it == m_agents.end())
    {
        respondJson(client, req, 404, Json::Value("no such agent"));
        return;
    }
    int lines = atoi(req.queryParam("lines").c_str());
    if (lines <= 0) lines = 200;
    if (lines > 5000) lines = 5000;

    string out;
    {
        // 启停阶段的少量记录(启动失败原因等)
        JZMutexLocker lock(&it->second->mutex);
        auto &logs = it->second->logs;
        size_t start = logs.size() > (size_t)lines ? logs.size() - lines : 0;
        for (size_t i = start; i < logs.size(); ++i)
        {
            out += logs[i];
            out += "\n";
        }
    }

    // 运行日志: cli 跑在本进程里, 回合诊断走 JZLog 的 Agent 模块(Agent-<日期>.log)。
    // 子进程时代的 stdout 整流已经不在了, 这里直接给文件末尾。
    {
        const string logDir = jzLogDirOf(m_config.logFile,
                                         JZPlatform::applicationDirPath() + "/data/logs");
        const string today = JZDateTime::currentDateTime().toString("yyyyMMdd");
        string text;
        if (!readTextFile(logDir + "/Agent-" + today + ".log", text))
            readTextFile(logDir + "/Agent.log", text);

        if (!text.empty())
        {
            // 只取末尾 lines 行
            std::vector<size_t> starts;
            size_t p = 0;
            while (p < text.size() && starts.size() <= (size_t)lines)
            {
                starts.push_back(p);
                size_t nl = text.find('\n', p);
                if (nl == string::npos)
                    break;
                p = nl + 1;
            }
            size_t from = starts.size() > (size_t)lines ? starts[starts.size() - lines] : 0;
            out += text.substr(from);
            if (!out.empty() && out.back() != '\n')
                out += "\n";
        }
    }
    respondText(client, req, 200, out);   // text/plain 直接给前端展示
}

void JZAgentCliServer::onApiAgentAction(JZHttpServerClient *client, const JZHttpRequest &req,
                                         const string &action)
{
    if (!requirePost(client, req))
        return;

    string name = req.queryParam("name");
    bool ok = false;
    string msg;
    if (name.empty())
        msg = "缺少 name";
    else if (action == "start")
    {
        ok = spawnAgent(name);
        msg = ok ? "已启动" : "启动失败/已在运行";
    }
    else if (action == "stop")
    {
        ok = stopAgent(name);
        msg = ok ? "已停止" : "未在运行";
    }
    else if (action == "restart")
    {
        ok = restartAgent(name);
        msg = ok ? "已重启" : "重启失败";
    }
    else
        msg = "未知动作: " + action;

    Json::Value j;
    j["ok"] = ok;
    j["msg"] = msg;
    respondJson(client, req, ok ? 200 : 400, j);
}

void JZAgentCliServer::onDashboard(JZHttpServerClient *client, const JZHttpRequest &req)
{
    // 默认打开用户上次打开的那台 agent: 地址里没带 id 时, 按 cookie 302 过去。
    // 不这么做的话, JS 只能"挑第一台在线的", 每个浏览器看到的都是同一台。
    // 只对真正的首页(/)做: 未知路径也走这个 handler 兜底, 它们不该被 302 成首页。
    if (req.path() == "/" && req.queryParam("id").empty())
    {
        string last = req.cookie("jz_agent");
        if (agentKnown(last))
        {
            string query = req.query();     // 保留原有参数(如 ?ui=mobile), 只补 id
            string loc = "/?";
            if (!query.empty())
                loc += query + "&";
            loc += "id=" + urlEncodeComponent(last);

            JZHttpRespone resp;
            resp.setStatusCode(302);
            resp.setHeader("Location", loc);
            resp.setHeader("Cache-Control", "no-store");   // 别让浏览器把这次跳转缓存住
            client->response(resp, req);
            return;
        }
    }

    // 首页 + 未知路径兜底: 手机浏览器给 mobile.html, 桌面给 index.html。
    // 鉴权目前没有(客户端证书已移除, token 待接入); 读盘 / no-cache / Vary 由 servePage 统一处理。
    servePage(client, req, isMobileBrowser(req) ? "mobile.html" : "index.html");
}

// ---------------------------------------------------------------------------
// 管理台网页: 从磁盘读, 不再内嵌 HTML/CSS/JS 字符串
//   web/(src) --CMake--> <exeDir>/server_web/ , 这里按 dashboardDir() 去读
//   好处: 改样式/脚本不用重编译; 现场能直接改文件排查(改坏了从包里重拷一份)。
// ---------------------------------------------------------------------------
string JZAgentCliServer::dashboardDir() const
{
    return JZPlatform::applicationDirPath() + "/server_web";
}

