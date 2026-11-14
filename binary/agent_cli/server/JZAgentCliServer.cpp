#include "JZAgentCliServer.h"

#include "JZNet/JZNetServer.h"   // 控制面(hub): cli 出站连进来的 JZNetServer
#include "JZAgentHubPacket.h"    // 控制面协议(cli <-> server)
#include "JZHttpServer.h"        // 后台网页 / 管理 API
#include "JZHttpStaticFiles.h"
#include "JZProcess.h"            // 拉起并监管 agent_cli 子进程
#include "JZDir.h"               // mkpath(data 目录)
#include "JZPlatform.h"          // applicationDirPath
#include "JZFileInfo.h"          // exists
#include "JZMutex.h"             // JZMutex / JZMutexLocker(日志跨线程读写保护)
#include "JZDateTime.h"          // currentMSecsSinceEpoch
#include "JZTimer.h"             // 任务超时(到点回 504 并请求 cli 取消)
#include "JZDebug.h"
#include "JZJson.h"              // Json::Value / FastWriter
#include "JZCodeTools.h"         // codeNormalizePath(切换工作目录时规范化路径)

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

// ===========================================================================
// 单个被管理 agent 的运行实例
// ===========================================================================
struct JZCliServerAgentInstance
{
    JZCliServerAgentConfig cfg;
    JZProcess *proc = nullptr;
    JZProcess::ProcessState state = JZProcess::NotRunning;
    int exitCode = 0;
    JZProcess::ExitStatus exitStatus = JZProcess::NormalExit;
    int64_t startTime = 0;
    int64_t stopTime = 0;

    mutable JZMutex mutex;          // 保护 logs / state / exitCode(读线程写, HTTP 线程读)
    std::deque<std::string> logs;   // 日志环形缓冲(已按行拆分)
    std::string recvBuf;            // 未完成的半行(仅读线程访问, 无需锁)
    size_t logMax = 3000;
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
    resp.setJson(Json::FastWriter().write(v));
    if (cookie)
        resp.setCookie(*cookie);
    client->response(resp, req);
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
static void respondJsonAsync(JZHttpServerClient *client, int code, const Json::Value &v)
{
    if (!client)
        return;
    JZHttpRespone resp;
    resp.setStatusCode(code);
    resp.setJson(Json::FastWriter().write(v));
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
// 文本太长就截断(事件序列要落盘, 一条巨型日志会撑爆 tasks.jsonl)。
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

static const char *stateString(JZProcess::ProcessState s)
{
    switch (s)
    {
        case JZProcess::Running:  return "running";
        case JZProcess::Starting: return "starting";
        default:                  return "stopped";
    }
}

static Json::Value agentToJson(JZCliServerAgentInstance *inst)
{
    Json::Value j;
    j["name"] = inst->cfg.name;
    j["enabled"] = inst->cfg.enabled;
    j["cmd"] = inst->cfg.cmd;
    j["args"] = Json::arrayValue;
    for (const auto &a : inst->cfg.args)
        j["args"].append(a);
    j["running"] = (inst->state == JZProcess::Running);
    j["state"] = stateString(inst->state);
    j["exitCode"] = inst->exitCode;
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
{
}

JZAgentCliServer::~JZAgentCliServer()
{
    for (auto &kv : m_agents)
    {
        if (kv.second->proc && kv.second->state == JZProcess::Running)
            kv.second->proc->killProcess();
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

    // 为每个 agent 配置建实例(进程随 server 一起存活), 并连好信号
    for (const auto &ac : m_config.agents)
    {
        auto *inst = new JZCliServerAgentInstance();
        inst->cfg = ac;
        inst->proc = new JZProcess(this);
        inst->proc->setProcessChannelMode(JZProcess::MergedChannels);   // stdout+stderr 合并
        if (!ac.dir.empty())
            inst->proc->setWorkingDirectory(ac.dir);

        // 收集日志(读线程触发); 跨线程写 logs, appendLog 内部加锁
        connect(&inst->proc->sigReadyReadStandardOutput,
                [this, inst]()
                {
                    JZByteArray b = inst->proc->readAllStandardOutput();
                    if (b.size() > 0)
                        appendLog(inst, str_fromUtf8(b));
                });
        // 启动成功
        connect(&inst->proc->sigStarted,
                [inst]()
                {
                    JZMutexLocker lock(&inst->mutex);
                    inst->state = JZProcess::Running;
                });
        // 结束: 把残留半行作为最后一行提交, 再更新状态
        connect(&inst->proc->sigFinished,
                [this, inst](int code, JZProcess::ExitStatus st)
                {
                    if (!inst->recvBuf.empty())
                    {
                        string tail = inst->recvBuf;
                        inst->recvBuf.clear();
                        appendLog(inst, tail + "\n");
                    }
                    JZMutexLocker lock(&inst->mutex);
                    inst->state = JZProcess::NotRunning;
                    inst->exitCode = code;
                    inst->exitStatus = st;
                    inst->stopTime = JZDateTime::currentMSecsSinceEpoch();
                });
        // 起不来(程序不存在等)
        connect(&inst->proc->sigErrorOccurred,
                [this, inst](JZProcess::ProcessError)
                {
                    appendLog(inst, "[error] 进程启动/运行失败\n");   // 内部加锁
                    JZMutexLocker lock(&inst->mutex);
                    inst->state = JZProcess::NotRunning;
                    inst->stopTime = JZDateTime::currentMSecsSinceEpoch();
                });

        m_agents[ac.name] = inst;
    }
    return true;
}

void JZAgentCliServer::start()
{
    m_startTime = JZDateTime::currentMSecsSinceEpoch();

    // 内置 agent: server 自己那个 JZAgent(可选)。先起它, 后面的工具才拿得到集群视图
    if (m_config.selfAgent.enabled)
    {
        m_selfAgent = new JZServerAgent(this);
        string err;
        if (!m_selfAgent->init(m_config.selfAgent, this, err))
        {
            jzWarning() << "内置 agent 启动失败:" << err;
            m_selfAgent->deleteLater();
            m_selfAgent = nullptr;
        }
        else
        {
            connect(&m_selfAgent->sigEvent, this, &JZAgentCliServer::onSelfAgentEvent);
            jzInfo() << "内置 agent 已就绪: id=" << m_config.selfAgent.agentId
                     << "  model=" << m_config.selfAgent.modelName
                     << "  root=" << m_config.selfAgent.rootDir;
        }
    }

    startHub();    // 控制面(JZNet): 等 agent_cli 出站连进来
    startHttp();   // 后台网页 + 管理 API
    for (const auto &kv : m_agents)
        if (kv.second->cfg.enabled)
            spawnAgent(kv.first);
}

// ---------------------------------------------------------------------------
// 控制面(hub): agent_cli 出站连进来(JZNet + token)
//   cli 是 client, 本进程是 server —— 内网机器不需要端口暴露/端口映射,
//   server 也不需要知道 cli 的地址(见 docs/agent-hub-design.md §2)。
// ---------------------------------------------------------------------------
void JZAgentCliServer::startHub()
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
        // 顺便当 SSE 心跳: 网页靠"20s 收不到任何帧"判断下行断了(前端 EVT_LAST)
        broadcastAgentEvent();
    });
    m_reapTimer->start(5000);

    if (!m_hub->listen(m_config.hub.bind, m_config.hub.port))
        jzWarning() << "控制面监听失败:" << m_config.hub.bind << ":" << m_config.hub.port;
    else
        jzInfo() << "控制面已启动(JZNet):" << m_config.hub.bind << ":" << m_config.hub.port
                 << (m_config.hub.token.empty() ? "  token=(无)" : "  token=(已启用)");
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
            m_hub->sendPacket(conn, &ack);
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
        m_hub->sendPacket(conn, &ack);

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
        agent.lastEventType = ev->type;
        agent.lastEventData = ev->data;
        recordEvent(ev->taskId, ev->type, ev->data);   // A2A: 事件进 Task.events + SSE 广播
        return;
    }

    if (type == HubPack_taskEnd)
    {
        JZHubPackTaskEnd *end = (JZHubPackTaskEnd *)pack.get();
        agent.busy = false;
        agent.taskId.clear();

        Json::Value result;
        if (!parseJsonText(end->result, result) || !result.isObject())
        {
            result = Json::Value(Json::objectValue);
            result["status"] = "failed";
            result["error"] = "cli 返回的任务结果不是合法 JSON";
        }
        // 先落记录: 即使提交方早就断了(超时/异步模式), 历史也要留全
        recordEnd(end->taskId, result);

        // 续会话: cli 把这一轮的对话快照带回来了, 按会话存一份(同一 contextId 覆盖)
        if (result.isMember("context"))
        {
            TaskRecord *rr = findTask(end->taskId);
            if (rr)
                saveContextFile(rr->contextId, result["context"]);
        }

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
        respondJsonAsync(pt.client, 200, rec ? taskJson(*rec) : Json::Value(Json::objectValue));
        jzInfo().noquote().nospace() << "任务完成: " << end->taskId << "  agent=" << pt.agentId
                                     << "  state=" << (rec ? rec->state : string(kStateUnknown));
        return;
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

        // 先落终态再取消: selfAgent.cancel() 会同步发 done, 不能让这次 done 抢先写成别的状态。
        const int secs = pt.timeoutMs / 1000;
        const std::string human =
            secs >= 60 ? (to_string(secs / 60) + " 分钟") : (to_string(secs) + " 秒");
        recordFail(taskId, kStateFailed, "任务无响应(" + human + "没有任何事件)");

        if (isSelfAgentId(pt.agentId))
        {
            if (m_selfAgent)
                m_selfAgent->cancel();
        }
        else
        {
            auto ai = m_connByAgent.find(pt.agentId);
            if (ai != m_connByAgent.end() && m_hub)
            {
                JZHubPackTaskCancel cancel;
                cancel.taskId = taskId;
                m_hub->sendPacket(ai->second, &cancel);
            }
        }

        // 回包与 GetTask 同形状(504 只是 HTTP 状态, body 仍是 Task)
        TaskRecord *rec = findTask(taskId);
        respondJsonAsync(pt.client, 504, rec ? taskJson(*rec) : Json::Value(Json::objectValue));
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
        respondJsonAsync(pt.client, 502, rec ? taskJson(*rec) : Json::Value(Json::objectValue));
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

    // 内置 agent: 与远端 cli 平级地出现在列表里(网页点进去看到的就是它)
    if (m_selfAgent)
    {
        Json::Value j(Json::objectValue);
        j["agentId"] = m_config.selfAgent.agentId;
        j["name"] = m_config.selfAgent.name;
        j["version"] = "builtin";
        j["rootDir"] = m_selfAgent->config().rootDir;
        j["project"] = m_selfAgent->config().rootDir;   // project 就是工作区根目录
        j["modelName"] = m_config.selfAgent.modelName;
        j["busy"] = m_selfAgent->busy();
        j["round"] = m_selfAgent->round();
        j["taskId"] = m_selfAgent->taskId();
        j["acceptTasks"] = true;
        j["builtin"] = true;
        j["lastSeen"] = (Json::Int64)JZDateTime::currentMSecsSinceEpoch();
        j["lastEventType"] = m_selfAgent->lastEventType();
        j["nodes"] = Json::Value(Json::arrayValue);
        arr.append(j);
    }
    return arr;
}

// ---------------------------------------------------------------------------
// 任务记录: server 侧历史
//   数据归属(§15): server 记"任务生命周期", cli 记"当前执行"; 历史必须在 server 落盘,
//   否则 cli 重启后提交方查不到自己交过的任务。JSONL 追加写, 载入时同 taskId 后者覆盖前者。
// ---------------------------------------------------------------------------
void JZAgentCliServer::loadTaskStore()
{
    const string dir = JZPlatform::applicationDirPath() + "/data";
    JZDir dataDir;
    dataDir.mkpath(dir);
    m_taskFile = dir + "/tasks.jsonl";
    // 会话上下文放在同目录的 ctx/ 下: 一个 contextId 一个文件, 只留最新一份
    m_ctxDir = dir + "/ctx";
    dataDir.mkpath(m_ctxDir);

    ifstream ifs(m_taskFile, ios::binary);
    if (!ifs)
        return;

    const long long now = (long long)JZDateTime::currentMSecsSinceEpoch();
    string line;
    int n = 0;
    while (getline(ifs, line))
    {
        if (line.empty())
            continue;
        Json::Value j;
        if (!parseJsonText(line, j) || !j.isObject())
            continue;
        // 只认 A2A Task 一种落盘格式; 不是这种形状的行直接跳过, 不做旧格式兼容
        TaskRecord r;
        r.taskId = j.get("id", "").asString();
        if (r.taskId.empty())
            continue;
        Json::Value meta = j.get("metadata", Json::Value(Json::objectValue));
        Json::Value st = j.get("status", Json::Value(Json::objectValue));
        Json::Value statusMsg = st.get("message", Json::Value(Json::objectValue));
        Json::Value statusParts = statusMsg.get("parts", Json::Value(Json::arrayValue));
        r.agentId = meta.get("agentId", "").asString();
        r.prompt = meta.get("prompt", "").asString();
        r.verifyCmd = meta.get("verifyCmd", "").asString();
        r.origin = meta.get("origin", "").asString();
        if (r.origin.empty())
            r.origin = kOriginUser;         // 旧记录没有来源字段: 当时的任务全部是人下发的
        r.parentTaskId = meta.get("parentTaskId", "").asString();
        r.state = st.get("state", kStateUnknown).asString();
        if (statusParts.isArray() && statusParts.size() > 0)
            r.error = statusParts[0].get("text", "").asString();
        r.lastEventType = meta.get("lastEventType", "").asString();
        r.rounds = meta.get("rounds", 0).asInt();
        r.round = meta.get("round", 0).asInt();
        r.startMs = (long long)meta.get("startMs", 0).asInt64();
        r.endMs = (long long)meta.get("endMs", 0).asInt64();
        r.contextId = j.get("contextId", "").asString();
        if (r.contextId.empty())
            r.contextId = "ctx-" + r.taskId;
        r.project = meta.get("project", "").asString();
        r.statusMs = (long long)meta.get("statusMs", r.endMs ? r.endMs : r.startMs).asInt64();
        r.events = stringsFromJson(meta.get("events", Json::Value(Json::arrayValue)));
        r.artifacts = stringsFromJson(j.get("artifacts", Json::Value(Json::arrayValue)));
        r.history = stringsFromJson(j.get("history", Json::Value(Json::arrayValue)));
        r.eventSeq = meta.get("eventSeq", (int)r.events.size()).asInt();
        if (m_tasks.find(r.taskId) == m_tasks.end())
            m_taskOrder.push_back(r.taskId);
        if (isWorkingState(r.state))
        {
            // 上次进程没等到回包就退出了: 结果不可知(不能假装还在跑)
            r.state = kStateUnknown;
            r.error = "server 重启, 结果未知";
            r.endMs = now;
        }
        m_tasks[r.taskId] = r;
        ++n;
    }
    while (m_taskOrder.size() > 500)
    {
        m_tasks.erase(m_taskOrder.front());
        m_taskOrder.erase(m_taskOrder.begin());
    }
    jzInfo().noquote().nospace() << "任务记录: 载入 " << n << " 条 (" << m_taskFile << ")";
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

void JZAgentCliServer::dropContextIfUnused(const string &contextId)
{
    if (contextId.empty())
        return;
    for (const auto &kv : m_tasks)
        if (kv.second.contextId == contextId)
            return;      // 这条会话还有任务, 上下文留着
    JZDir().remove(contextFile(contextId));
}

void JZAgentCliServer::saveTaskRecord(const TaskRecord &r)
{
    if (m_taskFile.empty())
        return;
    string line = Json::FastWriter().write(taskJson(r, -1));   // 落盘: 全量事件
    ofstream ofs(m_taskFile, ios::app | ios::binary);
    if (!ofs)
    {
        jzWarning() << "任务记录写不进去:" << m_taskFile;
        return;
    }
    ofs << line;   // FastWriter 自带行尾
}

void JZAgentCliServer::rewriteTaskStore()
{
    if (m_taskFile.empty())
        return;
    ofstream ofs(m_taskFile, ios::binary | ios::trunc);
    if (!ofs)
    {
        jzWarning() << "任务记录重写失败:" << m_taskFile;
        return;
    }
    // 保持 JSONL: 每条一行, 与追加写同格式
    for (const auto &id : m_taskOrder)
    {
        auto it = m_tasks.find(id);
        if (it == m_tasks.end())
            continue;
        string line = Json::FastWriter().write(taskJson(it->second, -1));   // 重写: 全量
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
            line.pop_back();
        ofs << line << "\n";
    }
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

    // 内置 agent 永远在线, 也放进同一张表
    if (m_selfAgent)
    {
        Json::Value j(Json::objectValue);
        j["agentId"] = m_config.selfAgent.agentId;
        j["name"] = m_config.selfAgent.name;
        j["version"] = "builtin";
        j["rootDir"] = m_selfAgent->config().rootDir;
        j["project"] = m_selfAgent->config().rootDir;   // project 就是工作区根目录
        j["modelName"] = m_config.selfAgent.modelName;
        j["firstSeen"] = (Json::Int64)m_startTime;
        j["connects"] = 1;
        j["builtin"] = true;
        j["online"] = true;
        j["busy"] = m_selfAgent->busy();
        j["taskId"] = m_selfAgent->taskId();
        j["round"] = m_selfAgent->round();
        j["lastSeen"] = (Json::Int64)JZDateTime::currentMSecsSinceEpoch();
        j["lastEventType"] = m_selfAgent->lastEventType();
        j["nodes"] = Json::Value(Json::arrayValue);
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
        if (isSelfAgentId(agentId))
            r.project = m_selfAgent ? m_selfAgent->config().rootDir : string();
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
    saveTaskRecord(r);
}

// 「接着说」: 往已有任务里追加一轮(同一个 taskId / contextId), 事件与 A2A history 都往后长。
// 任务记录只此一条, 所以前端聊天区天然连续; 执行仍是新的一次 run。
void JZAgentCliServer::recordContinue(const string &taskId, const Json::Value &body)
{
    TaskRecord *r = findTask(taskId);
    if (!r)
        return;
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
    flushOpenSegment(r);        // 收尾前把正在攒的 reply/thinking 段落成一条
    const string reply = result.get("agent_reply", "").asString();
    r->state = stateFromResult(result.get("status", "").asString());
    r->rounds = result.get("verify_rounds", 0).asInt();
    r->error = result.get("error", "").asString();
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

    broadcastTaskEvent(taskId, "artifact_update",
                       toJsonLine(jsonStringArray(r->artifacts)));
    finishTaskStreams(taskId);   // 终态: 补完整 Task 快照后关流
}

void JZAgentCliServer::recordFail(const string &taskId, const string &state, const string &error)
{
    TaskRecord *r = findTask(taskId);
    if (!r || !isWorkingState(r->state))
        return;
    flushOpenSegment(r);
    r->state = state;
    r->error = error;
    r->endMs = r->statusMs = (long long)JZDateTime::currentMSecsSinceEpoch();
    Json::Value tail(Json::objectValue);
    tail["state"] = state;
    tail["error"] = error;
    appendTaskEvent(r, "task_end", tail);
    saveTaskRecord(*r);
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
}

void JZAgentCliServer::appendTaskEvent(TaskRecord *r, const string &type, const Json::Value &data)
{
    if (!r)
        return;

    // reply/thinking 是逐 token 来的: 攒成一整段再落一条, 否则 events 全是碎片。
    if (type == "reply" || type == "thinking")
    {
        const string text = data.isString() ? data.asString()
                          : (data.isObject() ? data.get("text", "").asString() : string());
        if (r->openKind == type)
        {
            r->openText += text;
            // 一段很长时不憋着: 先落一次, 让中途看网页/落盘也能看到大部分内容
            if (r->openText.size() >= 8192)
                flushOpenSegment(r);
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
    // 推给 SSE 的是**这一条原始分片**(前端拿它只为"有新数据了"这个信号),
    // 不能用 r->events.back() —— reply/thinking 现在会合并, back() 未必是刚来的这条,
    // 而且新任务的第一条事件可能还没落(openText 正在攒), back() 会越界。
    // SSE 只是"有新数据了"的信号: 分片级别推送太密(一秒几百次), 同一任务 150ms 推一次就够,
    // 前端随后会自己拉详情/轮询补齐。终态强制推, 免得收尾那一帧被节流吞掉。
    Json::Value wire(Json::objectValue);
    wire["seq"] = r->eventSeq;
    wire["type"] = type;
    wire["data"] = v;
    const long long tk = JZPlatform::getTickCountMs();
    auto sit = m_sseTick.find(taskId);
    const bool forcePush = (type == "task_end" || type == "error" || type == "submitted");
    if (forcePush || sit == m_sseTick.end() || tk - sit->second >= 150)
    {
        m_sseTick[taskId] = tk;
        broadcastTaskEvent(taskId, "status_update", toJsonLine(wire));
    }
    // 逐条落盘会把 JSONL 撑成 O(n^2); 每 20 条 + 终态各存一次, 中途 crash 只丢尾部过程。
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
                                          const string &payload)
{
    // 网页常连(下行): 所有任务的事件都推一份, 帧里带 taskId, 前端自己筛。
    // 这条连接不随任务结束关闭 —— 关流/重连就是"连接中断"的来源。
    if (!m_pageStreams.empty())
    {
        // 常连对网页只是"这个任务有新数据了"的信号(前端只读 taskId), 别把整段原始分片
        // 再下发一遍: 一条 tool_result 可以到 96KB, 会随 150ms 节流反复推给每个页面。
        // /api/tasks/subscribe 那条流仍带完整 payload, 供 A2A 订阅者使用。
        Json::Value w(Json::objectValue);
        w["taskId"] = taskId;
        w["event"] = event;
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

// 对外只有这一种形状: A2A v1.0 Task。管理所需的额外信息一律放 metadata。
Json::Value JZAgentCliServer::taskJson(const TaskRecord &r, int evLimit) const
{
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
    meta["lastEventType"] = r.lastEventType;
    // 事件**全量落盘**, 但对外一次只回一页(默认最近 200 条, 起点对齐到 user_msg)。
    // 前端拿 eventsOffset > 0 就显示"加载更多", 走 /api/tasks/events 往前翻。
    // evLimit <= 0 表示全量(落盘/重写 tasks.jsonl 时用)。
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
        // 列表只要"最近发生什么"给左栏/摘要用: 30 条足够, 整表体积从几百 KB 降到几十 KB
        // (选中某条时前端会再用 /api/tasks/detail 取完整一页)
        arr.append(taskJson(ti->second, 30));
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
    flushOpenSegment(r);        // 正在攒的那段先落, 详情才是完整的
    respondJson(client, req, 200, taskJson(*r));
}

// 往前翻一页事件: GET /api/tasks/events?id=<taskId>&before=<offset>&limit=200
// 前端点"加载更多"时调用; offset 就是上一次拿到的 eventsOffset。
void JZAgentCliServer::onApiTaskEvents(JZHttpServerClient *client, const JZHttpRequest &req)
{
    TaskRecord *r = findTask(req.queryParam("id"));
    if (!r)
    {
        respondJson(client, req, 404, Json::Value("没有这个任务"));
        return;
    }
    SlowReqGuard slow("api/tasks/events");
    flushOpenSegment(r);

    const size_t total = r->events.size();
    size_t end = (size_t)atoi(req.queryParam("before").c_str());
    if (end == 0 || end > total)
        end = total;
    int limit = atoi(req.queryParam("limit").c_str());
    if (limit <= 0 || limit > 2000)
        limit = 1000;
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
    if (m_selfAgent && isSelfAgentId(id))
    {
        builtin = true;
        online = true;
        busy = m_selfAgent->busy();
        taskId = m_selfAgent->taskId();
        round = m_selfAgent->round();
        version = "builtin";
        rootDir = m_selfAgent->config().rootDir;
        modelName = m_config.selfAgent.modelName;
        description = "server 进程内置 agent(可管理集群)";
        lastSeen = JZDateTime::currentMSecsSinceEpoch();
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

    // 内置 agent 真正加载到的 skill(项目 .jzagent/skills + 全局 ~/.jzagent/skills),
    // 排障时一眼能看出"到底加载了哪些"
    if (builtin && m_selfAgent)
    {
        for (const string &sn : m_selfAgent->skillNames())
        {
            Json::Value k(Json::objectValue);
            k["id"] = "skill:" + sn;
            k["name"] = sn;
            k["description"] = "skill";
            k["tags"] = Json::Value(Json::arrayValue);
            k["tags"].append("skill");
            skills.append(k);
        }
    }
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
                        m_agents.find(id) != m_agents.end() ||
                        (m_selfAgent && isSelfAgentId(id));
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
    string path = req.queryParam("path");
    if (path.empty())
    {
        path = m_selfAgent ? m_selfAgent->config().rootDir : string();
        if (path.empty())
            path = JZDir::currentPath();
    }
    path = codeNormalizePath(path);

    JZFileInfo info(path);
    if (!info.exists() || !info.isDir())
    {
        respondJson(client, req, 404, Json::Value("目录不存在: " + path));
        return;
    }

    Json::Value out(Json::objectValue);
    out["path"] = path;

    // 父目录: 到盘符根("C:/")或 "/" 就没了
    string parent = path;
    while (parent.size() > 1 && parent.back() == '/')
        parent.pop_back();
    const size_t pos = parent.find_last_of('/');
    if (pos == 0)
        parent = "/";
    else if (pos == 2 && parent.size() >= 3 && parent[1] == ':')
        parent = parent.substr(0, 3);
    else if (pos == string::npos && parent.size() == 2 && parent[1] == ':')
        parent = string();                       // "C:" 就是 C 盘根, 没有上级
    else if (pos != string::npos)
        parent = parent.substr(0, pos);
    out["parent"] = (parent == path) ? string() : parent;

    vector<string> names = JZDir(path).entryList(vector<string>(), JZDir::Dirs | JZDir::NoDotAndDotDot);
    std::sort(names.begin(), names.end());
    Json::Value dirs(Json::arrayValue);
    for (const string &n : names)
    {
        Json::Value d(Json::objectValue);
        d["name"] = n;
        d["path"] = path + "/" + n;
        dirs.append(d);
    }
    out["dirs"] = dirs;

    // 文件(文件树的只读浏览): 名字/大小/修改时间。内容不在这里, 走 /api/fs/raw。
    vector<string> files = JZDir(path).entryList(vector<string>(), JZDir::Files | JZDir::NoDotAndDotDot);
    std::sort(files.begin(), files.end());
    Json::Value farr(Json::arrayValue);
    for (const string &n : files)
    {
        const string fp = path + "/" + n;
        JZFileInfo fi(fp);
        Json::Value f(Json::objectValue);
        f["name"] = n;
        f["path"] = fp;
        f["size"] = (Json::Int64)fi.size();
        f["mtime"] = (Json::Int64)fi.lastModified();
        farr.append(f);
        if (farr.size() >= 3000)      // 超大目录截断, 免得一次回几十 MB JSON
            break;
    }
    out["files"] = farr;

    // 根(盘符)列表: 网页上像 VSCode 的"打开文件夹"那样, 可以从别的盘开始。
    // 不引平台头文件, 直接试 A: .. Z: 是否存在(26 次 stat, 可忽略)。
    Json::Value drives(Json::arrayValue);
    for (char ch = 'A'; ch <= 'Z'; ++ch)
    {
        const string d = string(1, ch) + ":/";
        if (JZFileInfo(d).exists() && JZFileInfo(d).isDir())
            drives.append(d);
    }
    if (drives.empty())
        drives.append("/");
    out["drives"] = drives;

    // 主目录: 界面上放个"快捷入口"
    const char *home = getenv("USERPROFILE");
    if (!home || !*home)
        home = getenv("HOME");
    if (home && *home)
        out["home"] = codeNormalizePath(home);

    respondJson(client, req, 200, out);
}

// 文件内容(原始字节): GET /api/fs/raw?path=...
// 故意**不解码**: 后端不猜编码, 原样给字节, 由前端 TextDecoder 先试 UTF-8 再退 GB18030。
// 这样 Windows 上 GBK 的文件也不会被转坏, 也不需要引 iconv / MultiByteToWideChar。
void JZAgentCliServer::onApiFsRaw(JZHttpServerClient *client, const JZHttpRequest &req)
{
    SlowReqGuard slow("api/fs/raw");
    const string path = codeNormalizePath(req.queryParam("path"));
    JZFileInfo info(path);
    if (path.empty() || !info.exists() || info.isDir())
    {
        respondJson(client, req, 404, Json::Value("文件不存在: " + path));
        return;
    }

    static const int kMax = 1024 * 1024;      // 最多给前端 1MB
    JZFile file;
    if (!file.open(path, "rb"))
    {
        respondJson(client, req, 403, Json::Value("打不开(权限?): " + path));
        return;
    }
    JZByteArray data = file.read(kMax);
    file.close();

    JZHttpRespone resp;
    resp.setStatusCode(200);
    resp.setContentType("application/octet-stream");
    resp.setHeader("Cache-Control", "no-store");
    resp.setHeader("X-File-Size", std::to_string((long long)info.size()));
    resp.setHeader("X-File-Mtime", std::to_string((long long)info.lastModified()));
    resp.setBody(std::string(data.data(), (size_t)data.size()));
    client->response(resp, req);
}

// 工作区边界: p 落在 root 之内(root 本身也算)
static bool pathInRoot(const string &root, const string &p)
{
    return !root.empty() && !p.empty() &&
           (p == root ||
            (p.size() > root.size() && p.compare(0, root.size(), root) == 0 && p[root.size()] == '/'));
}

// 名字必须是单层(不许 / \ . ..) —— 挡掉 "../x" 这类穿越
static bool validEntryName(const string &name)
{
    return !name.empty() && name != "." && name != ".." &&
           name.find('/') == string::npos && name.find('\\') == string::npos;
}

// 父目录: "C:/a/b" -> "C:/a"; "C:/a" -> "C:/"; "C:/" -> ""
static string parentDirOf(const string &p)
{
    const size_t pos = p.find_last_of('/');
    if (pos == string::npos)
        return string();
    if (pos == 0)
        return "/";
    if (pos == 2 && p.size() >= 3 && p[1] == ':')
        return p.substr(0, 3);
    return p.substr(0, pos);
}

// 新建: POST /api/fs/create {root, dir, name, kind: "dir"|"file"}
//
// 边界 = **工作区内**(与 cli 的区域模型一致: 全盘可读, 工作区可写, 授信目录只读) ——
// 这个规则三档(strict/standard/loose)完全一样, 见 JZVirtualTerminalConfig::applyProfile,
// 所以不需要把 cli 的 profile 传上来。
//
// root 为什么由前端带: 网页上切工作区只写浏览器的 localStorage, server 并不知道
// "这个人现在看的是哪个目录"。root 只用来做前缀判定, 不参与别的逻辑。
void JZAgentCliServer::onApiFsCreate(JZHttpServerClient *client, const JZHttpRequest &req)
{
    SlowReqGuard slow("api/fs/create");

    Json::Value body;
    if (!parseJsonText(string(req.body().data(), req.body().size()), body) || !body.isObject())
    {
        respondJson(client, req, 400, Json::Value("body 必须是 JSON 对象"));
        return;
    }

    const string root = codeNormalizePath(body.get("root", "").asString());
    const string dir  = codeNormalizePath(body.get("dir", "").asString());
    const string name = body.get("name", "").asString();
    const string kind = body.get("kind", "dir").asString();
    if (root.empty() || dir.empty() || name.empty())
    {
        respondJson(client, req, 400, Json::Value("缺少 root / dir / name"));
        return;
    }
    // 名字必须是单层: 不许带分隔符, 不许 . / ..(挡掉 ../ 这类穿越)
    if (name.find('/') != string::npos || name.find('\\') != string::npos ||
        name == "." || name == "..")
    {
        respondJson(client, req, 400, Json::Value("名字不能带路径分隔符, 也不能是 . 或 .."));
        return;
    }
    // 边界: dir 必须在 root 之内(root 本身也算)
    const bool inside = (dir == root) ||
        (dir.size() > root.size() && dir.compare(0, root.size(), root) == 0 && dir[root.size()] == '/');
    if (!inside)
    {
        respondJson(client, req, 403,
                    Json::Value("只能在工作区内新建\n工作区: " + root + "\n目标: " + dir));
        return;
    }

    JZFileInfo dirInfo(dir);
    if (!dirInfo.exists() || !dirInfo.isDir())
    {
        respondJson(client, req, 404, Json::Value("目录不存在: " + dir));
        return;
    }

    const string target = dir + "/" + name;
    if (JZFileInfo(target).exists())
    {
        respondJson(client, req, 400, Json::Value("已存在: " + name));
        return;
    }

    bool ok = false;
    if (kind == "file")
    {
        JZFile f;                       // "wb" 就是"建一个空文件"
        ok = f.open(target, "wb");
        if (ok)
            f.close();
    }
    else
    {
        ok = JZDir(dir).mkpath(target);
    }
    if (!ok)
    {
        respondJson(client, req, 500, Json::Value("创建失败(权限?): " + target));
        return;
    }

    Json::Value out(Json::objectValue);
    out["ok"] = true;
    out["path"] = target;
    out["kind"] = (kind == "file" ? "file" : "dir");
    respondJson(client, req, 200, out);
}

// 重命名: POST /api/fs/rename {root, path, name}
// 只改名字, 不移动目录(新名字拼在同一个父目录下); 边界与新建一致。
void JZAgentCliServer::onApiFsRename(JZHttpServerClient *client, const JZHttpRequest &req)
{
    SlowReqGuard slow("api/fs/rename");

    Json::Value body;
    if (!parseJsonText(string(req.body().data(), req.body().size()), body) || !body.isObject())
    {
        respondJson(client, req, 400, Json::Value("body 必须是 JSON 对象"));
        return;
    }
    const string root = codeNormalizePath(body.get("root", "").asString());
    const string path = codeNormalizePath(body.get("path", "").asString());
    const string name = body.get("name", "").asString();

    if (root.empty() || path.empty() || name.empty())
    {
        respondJson(client, req, 400, Json::Value("缺少 root / path / name"));
        return;
    }
    if (!pathInRoot(root, path))
    {
        respondJson(client, req, 403, Json::Value("只能改工作区内的东西\n工作区: " + root));
        return;
    }
    if (path == root)
    {
        respondJson(client, req, 400, Json::Value("不能重命名工作区根目录"));
        return;
    }
    if (!validEntryName(name))
    {
        respondJson(client, req, 400, Json::Value("名字不能带路径分隔符, 也不能是 . 或 .."));
        return;
    }
    JZFileInfo info(path);
    if (!info.exists())
    {
        respondJson(client, req, 404, Json::Value("不存在: " + path));
        return;
    }
    const string parent = parentDirOf(path);
    const string target = parent + "/" + name;
    if (target == path)
    {
        Json::Value out(Json::objectValue);
        out["ok"] = true;
        out["path"] = target;
        respondJson(client, req, 200, out);
        return;
    }
    if (JZFileInfo(target).exists())
    {
        respondJson(client, req, 400, Json::Value("已存在: " + name));
        return;
    }

    string err;
    const bool ok = info.isDir() ? JZDir::rename(path, target, &err)
                                 : JZFile::rename(path, target, &err);
    if (!ok)
    {
        respondJson(client, req, 500,
                    Json::Value("重命名失败" + (err.empty() ? string() : (": " + err))));
        return;
    }
    Json::Value out(Json::objectValue);
    out["ok"] = true;
    out["path"] = target;
    respondJson(client, req, 200, out);
}

// 删除: POST /api/fs/delete {root, path}
// 目录**递归删除**(页面上会先确认); 工作区根目录不许删。
void JZAgentCliServer::onApiFsDelete(JZHttpServerClient *client, const JZHttpRequest &req)
{
    SlowReqGuard slow("api/fs/delete");

    Json::Value body;
    if (!parseJsonText(string(req.body().data(), req.body().size()), body) || !body.isObject())
    {
        respondJson(client, req, 400, Json::Value("body 必须是 JSON 对象"));
        return;
    }
    const string root = codeNormalizePath(body.get("root", "").asString());
    const string path = codeNormalizePath(body.get("path", "").asString());

    if (root.empty() || path.empty())
    {
        respondJson(client, req, 400, Json::Value("缺少 root / path"));
        return;
    }
    if (!pathInRoot(root, path))
    {
        respondJson(client, req, 403, Json::Value("只能删工作区内的东西\n工作区: " + root));
        return;
    }
    if (path == root)
    {
        respondJson(client, req, 400, Json::Value("不能删除工作区根目录"));
        return;
    }
    JZFileInfo info(path);
    if (!info.exists())
    {
        respondJson(client, req, 404, Json::Value("不存在: " + path));
        return;
    }
    const bool isDir = info.isDir();
    const bool ok = isDir ? JZDir(path).removeRecursively() : JZFile::remove(path);
    if (!ok)
    {
        respondJson(client, req, 500, Json::Value("删除失败(权限?): " + path));
        return;
    }
    Json::Value out(Json::objectValue);
    out["ok"] = true;
    out["path"] = path;
    out["dir"] = isDir;
    respondJson(client, req, 200, out);
}

// 主机视图: GET /api/projects
// 一次返回「每个 agent(在线与否 + 根目录) + 它用过的目录」。
// 目录从任务记录里聚合(不再让前端拉 tasks 全量 —— 那玩意儿带 events, 500 条能有几 MB)。
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
    if (m_selfAgent)
    {
        Agg &g = aggs[m_config.selfAgent.agentId];
        g.online = true;
        g.rootDir = m_selfAgent->config().rootDir;
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
        j["kind"] = "process";
        j["state"] = (int)inst->state;
        j["startTime"] = (Json::Int64)inst->startTime;
        j["exitCode"] = inst->exitCode;
        j["cmd"] = inst->cfg.cmd;
        j["nodes"] = Json::Value(Json::arrayValue);
        out["kind"] = "process";
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
    if (isSelfAgentId(id))
        return true;
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

    string id = req.queryParam("agentId");
    if (id.empty())
        id = req.queryParam("id");
    if (id.empty())
    {
        respondJson(client, req, 400, Json::Value("缺少 agentId"));
        return;
    }
    if (isSelfAgentId(id))
    {
        respondJson(client, req, 400,
                    Json::Value("内置 agent 不能删除(它跟着 selfAgent.enabled 一起存在)"));
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
bool JZAgentCliServer::isSelfAgentId(const string &id) const
{
    return m_selfAgent && !m_config.selfAgent.agentId.empty() && id == m_config.selfAgent.agentId;
}

int JZAgentCliServer::submitTask(const Json::Value &body, JZHttpServerClient *waiter,
                                 Json::Value &ack, string &error,
                                 const string &origin, const string &parentTaskId)
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
    if (agentId.empty() && m_onlineByConn.size() == 1 && !m_selfAgent)
        agentId = m_onlineByConn.begin()->second.agentId;   // 只有一台就默认它
    if (agentId.empty())
    {
        error = "缺少 agentId(当前在线 " + to_string(m_onlineByConn.size()) + " 台"
                + (m_selfAgent ? ", 另有内置 server agent" : "") + ")";
        ack["error"] = error;
        return 400;
    }

    Json::Value cliBody(Json::objectValue);
    cliBody["prompt"] = prompt;
    // ⚠ 以前这里只搬了 prompt/verify_cmd 等字段, images 被**静默丢掉** —— 网页传了图、cli 也支持,
    //   但中间这一层没转, 模型只看到文字(2026-11-12 实测: 模型跑去 workspace 里找文件猜内容)。
    if (!images.isNull())
        cliBody["images"] = images;
    cliBody["verify_cmd"] = body.get("verifyCmd", "").asString();
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
            error = "任务 " + contId + " 还在执行, 等它结束再接着说";
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

    // ---- 内置 agent: 任务在 server 进程内跑 ----
    if (isSelfAgentId(agentId))
    {
        if (m_selfAgent->busy())
        {
            error = "内置 agent 正忙: " + m_selfAgent->taskId();
            ack["error"] = error;
            ack["taskId"] = m_selfAgent->taskId();
            return 409;
        }

        // 内置 agent 的终端/工具绑在一个目录上: 目录变了要整体重建(忙时 setRootDir 会拒)
        if (!wantRoot.empty())
        {
            string dirError;
            if (!m_selfAgent->setRootDir(wantRoot, dirError))
            {
                error = dirError;
                ack["error"] = error;
                return 409;
            }
        }

        if (contRec)
            recordContinue(taskId, body);
        else
            recordCreate(taskId, agentId, body, origin, parentTaskId);
        PendingTask selfPending;
        selfPending.taskId = taskId;
        selfPending.agentId = agentId;
        selfPending.client = waiter;
        selfPending.startMs = JZDateTime::currentMSecsSinceEpoch();
        selfPending.lastEventMs = JZPlatform::getTickCountMs();
        selfPending.timeoutMs = timeoutMs;
        m_pendingTasks[taskId] = selfPending;
        // 续会话: 这条会话的上下文文件(没有 = 新会话, 从零开始)。
        // contextId 由 recordCreate 定下来(请求没带就是 ctx-<taskId>), 所以从记录里取。
        TaskRecord *rec = findTask(taskId);
        m_selfAgent->submit(taskId, prompt, body.get("verifyCmd", "").asString(),
                            loadContextFile(rec ? rec->contextId : string()));
        ack = taskJson(*findTask(taskId));      // SendMessage 的回包就是一个 Task
        jzInfo().noquote().nospace() << "任务下发(内置): " << taskId;
        return waiter ? 200 : 202;
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
    if (TaskRecord *rec0 = findTask(taskId))
    {
        Json::Value ctx = loadContextFile(rec0->contextId);
        if (ctx.isObject())
        {
            cliBody["context"] = ctx;
            jzInfo() << "续会话: " << taskId << " 带上 " << ctx["history"].size() << " 条历史";
        }
    }

    JZHubPackTaskSubmit submit;
    submit.taskId = taskId;
    string payload = Json::FastWriter().write(cliBody);   // hub 下游是 cli 的 /task 载荷
    while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r'))
        payload.pop_back();
    submit.body = payload;

    PendingTask pending;
    pending.taskId = taskId;
    pending.agentId = agentId;
    pending.client = waiter;
    pending.startMs = JZDateTime::currentMSecsSinceEpoch();
    pending.lastEventMs = JZPlatform::getTickCountMs();
    pending.timeoutMs = timeoutMs;
    m_pendingTasks[taskId] = pending;

    agent.busy = true;
    agent.taskId = taskId;
    agent.round = 0;

    m_hub->sendPacket(ai->second, &submit);
    jzInfo().noquote().nospace() << "任务下发: " << taskId << " -> " << agentId;

    ack = taskJson(*findTask(taskId));          // SendMessage 的回包就是一个 Task
    return waiter ? 200 : 202;
}

int JZAgentCliServer::submitTaskForTool(const Json::Value &body, Json::Value &ack, string &error)
{
    // 内置 agent 用工具派出去的活: 记成 agent 来源, 并挂到当前这条任务(它才是用户看到的那条)
    const string parent = m_selfAgent ? m_selfAgent->taskId() : string();
    return submitTask(body, nullptr, ack, error, kOriginAgent, parent);
}

// 内置 agent 的事件流: done = 收口(落记录 + 回等待方), 其余只更新"最近事件"
void JZAgentCliServer::onSelfAgentEvent(const JZServerAgentEvent &ev)
{
    if (ev.type == "done")
    {
        Json::Value result;
        if (!parseJsonText(ev.data, result) || !result.isObject())
        {
            result = Json::Value(Json::objectValue);
            result["status"] = "failed";
            result["error"] = "内置 agent 的结果不是合法 JSON";
        }
        result["taskId"] = Json::Value(ev.taskId);
        // 这一轮的对话快照覆盖写进 ctx/<contextId>.json: 同一会话的下一条任务据此续聊。
        // (只留最新一份, 所以不会随任务数膨胀)
        if (m_selfAgent)
        {
            if (TaskRecord *tr = findTask(ev.taskId))
                saveContextFile(tr->contextId, m_selfAgent->lastContextJson());
        }
        recordEnd(ev.taskId, result);

        auto pit = m_pendingTasks.find(ev.taskId);
        if (pit != m_pendingTasks.end())
        {
            PendingTask pt = pit->second;
            m_pendingTasks.erase(pit);
            TaskRecord *rec = findTask(ev.taskId);
            respondJsonAsync(pt.client, 200, rec ? taskJson(*rec) : Json::Value(Json::objectValue));
        }
        TaskRecord *rec = findTask(ev.taskId);
        jzInfo().noquote().nospace() << "内置 agent 任务结束: " << ev.taskId
                                     << "  state=" << (rec ? rec->state : string(kStateUnknown));
        return;
    }
    recordEvent(ev.taskId, ev.type, ev.data);
}

// ---------------------------------------------------------------------------
// A2A SendMessage(POST /api/tasks): 默认阻塞到终态; returnImmediately=true 立即回 Task。
// ---------------------------------------------------------------------------
void JZAgentCliServer::onApiTasks(JZHttpServerClient *client, const JZHttpRequest &req)
{
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
    const int code = submitTask(body, waiter, ack, error);
    if (code == 200)
        return;   // 同步模式: 回包在 taskEnd / done 时发出
    respondJson(client, req, code, ack);
}

void JZAgentCliServer::onApiTaskCancel(JZHttpServerClient *client, const JZHttpRequest &req)
{

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

    if (isSelfAgentId(pit->second.agentId))
    {
        if (m_selfAgent)
            m_selfAgent->cancel();
    }
    else
    {
        auto ai = m_connByAgent.find(pit->second.agentId);
        if (ai != m_connByAgent.end() && m_hub)
        {
            JZHubPackTaskCancel cancel;
            cancel.taskId = taskId;
            m_hub->sendPacket(ai->second, &cancel);
        }
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
            if (isSelfAgentId(agentId))
            {
                if (m_selfAgent)
                    m_selfAgent->cancel();
            }
            else
            {
                auto ai = m_connByAgent.find(agentId);
                if (ai != m_connByAgent.end() && m_hub)
                {
                    JZHubPackTaskCancel cancel;
                    cancel.taskId = tid;
                    m_hub->sendPacket(ai->second, &cancel);
                }
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
void JZAgentCliServer::startHttp()
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
    if (m_config.http.bind != "127.0.0.1" && m_config.http.bind != "localhost")
        jzWarning() << "管理台/API 暂未做应用层鉴权(客户端证书已移除, token 待接入): "
                    << m_config.http.bind << ":" << m_config.http.port << " 可被同网段任意访问";

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
        jzWarning() << "管理网页监听失败:" << m_config.http.bind << ":" << m_config.http.port;
    else
        jzInfo() << "管理网页已启动: " << (m_config.http.sslEnabled ? "https://" : "http://")
                 << m_config.http.bind << ":" << m_config.http.port
                 << (m_config.http.sslEnabled ? "  (单向 TLS, 无客户端认证)" : "  (明文!)");
}

// ---------------------------------------------------------------------------
// agent 管理
// ---------------------------------------------------------------------------
string JZAgentCliServer::resolveProgram(const JZCliServerAgentConfig &cfg) const
{
    string program = cfg.cmd;
    if (program.empty() || program == "agent_cli")
    {
        program = JZPlatform::applicationDirPath() + "/agent_cli";
#ifdef _WIN32
        if (!JZFileInfo::exists(program))
            program += ".exe";
#endif
    }
    return program;
}

bool JZAgentCliServer::spawnAgent(const string &name)
{
    auto it = m_agents.find(name);
    if (it == m_agents.end())
        return false;
    JZCliServerAgentInstance *inst = it->second;
    if (inst->state == JZProcess::Running || inst->state == JZProcess::Starting)
        return false;   // 已在跑

    string program = resolveProgram(inst->cfg);
    if (!inst->proc->start(program, inst->cfg.args))
    {
        appendLog(inst, "[error] 启动失败: " + program + "\n");
        return false;
    }
    inst->state = JZProcess::Starting;   // start 成功仅表示进程创建成功, Running 等 sigStarted
    inst->startTime = JZDateTime::currentMSecsSinceEpoch();
    inst->stopTime = 0;
    inst->exitCode = 0;
    jzInfo() << "agent 启动:" << name << "  cmd=" << program;
    return true;
}

bool JZAgentCliServer::stopAgent(const string &name)
{
    auto it = m_agents.find(name);
    if (it == m_agents.end())
        return false;
    JZCliServerAgentInstance *inst = it->second;
    if (inst->state != JZProcess::Running && inst->state != JZProcess::Starting)
        return false;
    inst->proc->terminate();
    jzInfo() << "agent 停止:" << name;
    return true;
}

bool JZAgentCliServer::restartAgent(const string &name)
{
    auto it = m_agents.find(name);
    if (it == m_agents.end())
        return false;
    JZCliServerAgentInstance *inst = it->second;
    if (inst->state == JZProcess::Running || inst->state == JZProcess::Starting)
    {
        inst->proc->killProcess();
        inst->proc->waitForFinished(2000);
    }
    {
        JZMutexLocker lock(&inst->mutex);
        inst->state = JZProcess::NotRunning;
        inst->recvBuf.clear();
    }
    return spawnAgent(name);
}

// ---------------------------------------------------------------------------
// 日志收集(读线程调用): 拆行入环形缓冲
// ---------------------------------------------------------------------------
void JZAgentCliServer::appendLog(JZCliServerAgentInstance *inst, const string &chunk)
{
    inst->recvBuf += chunk;
    size_t pos = 0;
    while (true)
    {
        size_t nl = inst->recvBuf.find('\n', pos);
        if (nl == string::npos)
            break;
        string line = inst->recvBuf.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        {
            JZMutexLocker lock(&inst->mutex);
            inst->logs.push_back(line);
            while (inst->logs.size() > inst->logMax)
                inst->logs.pop_front();
        }
        pos = nl + 1;
    }
    inst->recvBuf = inst->recvBuf.substr(pos);
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
        JZMutexLocker lock(&it->second->mutex);
        auto &logs = it->second->logs;
        size_t start = logs.size() > (size_t)lines ? logs.size() - lines : 0;
        for (size_t i = start; i < logs.size(); ++i)
        {
            out += logs[i];
            out += "\n";
        }
    }
    respondText(client, req, 200, out);   // text/plain 直接给前端展示
}

void JZAgentCliServer::onApiAgentAction(JZHttpServerClient *client, const JZHttpRequest &req,
                                         const string &action)
{
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

