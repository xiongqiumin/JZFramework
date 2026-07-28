#include "JZJSWebApp.h"

#include <algorithm>
#include <chrono>

#include "JZLog.h"
#include "JZHttpRespone.h"
#include "JZJSBind.h"

using namespace std;
using namespace jsbind;

JZJSWebApp* JZJSWebApp::s_instance = nullptr;

namespace
{
// ---- 工具 ----
int64_t nowMs()
{
    auto now = chrono::system_clock::now().time_since_epoch();
    return chrono::duration_cast<chrono::milliseconds>(now).count();
}

string jsonEscape(const string& in)
{
    string out;
    for (char c : in)
    {
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out += c;
        }
    }
    return out;
}

string getExceptionText(JSContext* ctx, JSValue exc)
{
    JSValue msg = JS_GetPropertyStr(ctx, exc, "message");
    const char* m = JS_ToCString(ctx, msg);
    string s = m ? m : "";
    JS_FreeCString(ctx, m);
    JS_FreeValue(ctx, msg);
    return s;
}

// query 字符串 -> Json::Value 对象：a=1&b=2 -> { a: "1", b: "2" }
Json::Value parseQueryToJson(const string& query)
{
    Json::Value obj;
    if (query.empty())
        return obj;
    size_t start = 0;
    while (start <= query.size())
    {
        size_t amp = query.find('&', start);
        string pair = (amp == string::npos) ? query.substr(start) : query.substr(start, amp - start);
        if (!pair.empty())
        {
            size_t eq = pair.find('=');
            string k = (eq == string::npos) ? pair : pair.substr(0, eq);
            string v = (eq == string::npos) ? "" : pair.substr(eq + 1);
            obj[k] = v;
        }
        if (amp == string::npos)
            break;
        start = amp + 1;
    }
    return obj;
}
}  // namespace

// ---------------------------------------------------------------------------
// JZJSWebClient —— 请求/响应桥接对象（绑定为 JS 的 WebClient 类）
// ---------------------------------------------------------------------------
std::string JZJSWebClient::method() const { return req.methodString(); }
std::string JZJSWebClient::path() const { return req.path(); }

std::string JZJSWebClient::url() const
{
    return req.path() + (req.query().empty() ? "" : "?" + req.query());
}

std::string JZJSWebClient::bodyStr() const
{
    return string(req.body().data(), req.body().size());
}

Json::Value JZJSWebClient::query() const { return parseQueryToJson(req.query()); }

Json::Value JZJSWebClient::params() const
{
    Json::Value obj;
    return obj;   // 由 JS 自行路由，无框架层路径参数
}

Json::Value JZJSWebClient::headersObj() const
{
    Json::Value obj;
    for (auto& h : req.headers())
        obj[h.first] = h.second;
    return obj;
}

void JZJSWebClient::respond(int s, const std::string& ct, const std::string& b)
{
    if (settled)
        return;
    status = s;
    contentType = ct;
    body = b;
    settled = true;
}

void JZJSWebClient::send(const Json::Value& data)
{
    if (settled)
        return;
    if (data.type() == Json::stringValue)
        text(data.asString());
    else
        json(data);
}

void JZJSWebClient::json(const Json::Value& data)
{
    if (settled)
        return;
    Json::FastWriter writer;
    body = writer.write(data);
    if (!body.empty() && body.back() == '\n')
        body.pop_back();
    contentType = "application/json; charset=utf-8";
    settled = true;
}

void JZJSWebClient::text(const std::string& t)
{
    if (settled)
        return;
    body = t;
    contentType = "text/plain; charset=utf-8";
    settled = true;
}

void JZJSWebClient::html(const std::string& h)
{
    if (settled)
        return;
    body = h;
    contentType = "text/html; charset=utf-8";
    settled = true;
}

void JZJSWebClient::setStatus(int code)
{
    if (!settled)
        status = code;
}

void JZJSWebClient::setHeader(const std::string& name, const std::string& value)
{
    if (!settled)
        headers[name] = value;
}

// ---------------------------------------------------------------------------
// JZJSWebApp
// ---------------------------------------------------------------------------
JZJSWebApp::JZJSWebApp(JZObject* parent)
    : JZObject(parent)
{
}

JZJSWebApp::~JZJSWebApp()
{
    shutdown();
}

bool JZJSWebApp::init(const std::string& appDir)
{
    s_instance = this;

    m_engine = new JZJSEngine(this);
    m_engine->init();
    m_engine->setWorkDir(appDir);

    registerClientClass();
    registerGlobals();

    // 注入 fetch，更接近 Node 环境
    m_http = new JZJSHttpModule(this);
    m_http->initEngine(m_engine);

    // 自动加载 scripts 下的所有 .js（按文件名排序，确定性加载顺序）
    string scriptsDir = appDir + "/scripts";
    JZDir dir(scriptsDir);
    if (dir.exists())
    {
        auto files = dir.entryList({ "*.js" }, JZDir::Files, JZDir::SortName);
        for (auto& f : files)
        {
            string full = dir.absoluteFilePath(f);
            LOG_I("load script: " + full);
            if (!m_engine->load(full))
                LOG_E("load script failed: " + full);
        }
    }
    else
    {
        LOG_I("no scripts dir: " + scriptsDir);
    }

    // 引擎自身的定时器负责执行 setTimeout / Promise 续体（会调用 client.* 回写响应）
    m_engine->start();
    // 本对象的定时器负责轮询：把已结算/超时的挂起请求发送出去
    startTimer(10);
    return true;
}

void JZJSWebApp::shutdown()
{
    if (!m_engine)
        return;

    // 释放挂起请求持有的 JS 引用；JZJSWebClient 由引擎 GC 回收（调用 destory 即 delete）
    for (auto& jsClient : m_pending)
        JS_FreeValue(m_engine->context(), jsClient);
    m_pending.clear();

    m_engine->stop();
    m_engine->deinit();
    m_engine = nullptr;
    m_http = nullptr;
    s_instance = nullptr;
}

void JZJSWebApp::registerClientClass()
{
    ClassBind<JZJSWebClient> wb("WebClient");

    // 只读请求字段（提供空 write 以避免 registClass 对空 write 的断言/解引用）
    auto ignoreStr = [](JZJSWebClient*, std::string) {};
    auto ignoreJson = [](JZJSWebClient*, Json::Value) {};
    wb.defProperty("method", [](JZJSWebClient* c) -> std::string { return c->method(); }, ignoreStr);
    wb.defProperty("path", [](JZJSWebClient* c) -> std::string { return c->path(); }, ignoreStr);
    wb.defProperty("apiPath", [](JZJSWebClient* c) -> std::string { return c->apiPathStr(); }, ignoreStr);
    wb.defProperty("url", [](JZJSWebClient* c) -> std::string { return c->url(); }, ignoreStr);
    wb.defProperty("body", [](JZJSWebClient* c) -> std::string { return c->bodyStr(); }, ignoreStr);
    wb.defProperty("query", [](JZJSWebClient* c) -> Json::Value { return c->query(); }, ignoreJson);
    wb.defProperty("params", [](JZJSWebClient* c) -> Json::Value { return c->params(); }, ignoreJson);
    wb.defProperty("headers", [](JZJSWebClient* c) -> Json::Value { return c->headersObj(); }, ignoreJson);

    // 响应方法
    wb.def("respond", &JZJSWebClient::respond);
    wb.def("send", &JZJSWebClient::send);
    wb.def("json", &JZJSWebClient::json);
    wb.def("text", &JZJSWebClient::text);
    wb.def("html", &JZJSWebClient::html);
    wb.def("status", &JZJSWebClient::setStatus);
    wb.def("set", &JZJSWebClient::setHeader);
    wb.def("header", &JZJSWebClient::setHeader);

    m_engine->registClass(wb.classDefine());

    // 记录 JSClassID，供后续 JS_GetOpaque 取回 C++ 对象
    int type = m_engine->ctypeidToType(typeid(JZJSWebClient).name());
    m_clientClassId = m_engine->typeToJSType(type);
}

void JZJSWebApp::registerGlobals()
{
    auto ctx = m_engine->context();
    // 预声明全局 onApiCall（初始 undefined）。JS 通过 `onApiCall = function(client){...}` 注册。
    JS_SetPropertyStr(ctx, m_engine->globalObject(), "onApiCall", JS_UNDEFINED);
}

JSValue JZJSWebApp::buildClient(JZHttpServerClient* client, const JZHttpRequest& req)
{
    JZJSWebClient* c = new JZJSWebClient();
    c->client = client;
    c->req = req;
    c->apiPath = "/" + req.param("rest");   // 剥离 /api 前缀后的子路径
    c->expireMs = nowMs() + 30000;

    int type = m_engine->ctypeidToType(typeid(JZJSWebClient).name());
    return m_engine->createValue(type, c);   // 包装 c；GC 负责 delete
}

void JZJSWebApp::dispatch(JZHttpServerClient* client, const JZHttpRequest& req)
{
    auto ctx = m_engine->context();

    JSValue onApiCall = JS_GetPropertyStr(ctx, m_engine->globalObject(), "onApiCall");
    if (!JS_IsFunction(ctx, onApiCall))
    {
        JS_FreeValue(ctx, onApiCall);
        JZHttpRespone response;
        response.setStatusCode(500);
        response.setContentType("text/plain; charset=utf-8");
        response.setBody("onApiCall is not registered");
        client->response(response.toByteArray());
        return;
    }

    JSValue jsClient = buildClient(client, req);   // ref = 1
    JZJSWebClient* c = (JZJSWebClient*)JS_GetOpaque(jsClient, m_clientClassId);

    JSValue args[1] = { jsClient };
    JSValue ret = JS_Call(ctx, onApiCall, JS_UNDEFINED, 1, args);
    JS_FreeValue(ctx, onApiCall);

    if (JS_IsException(ret))
    {
        JSValue exc = JS_GetException(ctx);
        c->status = 500;
        c->contentType = "application/json; charset=utf-8";
        c->body = "{\"error\":\"" + jsonEscape(getExceptionText(ctx, exc)) + "\"}";
        c->settled = true;
        JS_FreeValue(ctx, exc);
    }
    JS_FreeValue(ctx, ret);

    if (c->settled)
    {
        finalizeClient(jsClient);   // 同步 handler：发送并释放引用（c 可能随即被 GC 回收）
    }
    else
    {
        m_pending.push_back(JS_DupValue(ctx, jsClient));   // 持有独立引用
        JS_FreeValue(ctx, jsClient);
    }
}

void JZJSWebApp::sendResponse(JZJSWebClient* c)
{
    JZHttpRespone response;
    response.setStatusCode(c->status);
    if (!c->contentType.empty())
        response.setContentType(c->contentType);
    for (auto& h : c->headers)
        response.setHeader(h.first, h.second);
    response.setBody(c->body);
    c->client->response(response.toByteArray());
}

void JZJSWebApp::finalizeClient(JSValue jsClient)
{
    JZJSWebClient* c = (JZJSWebClient*)JS_GetOpaque(jsClient, m_clientClassId);
    sendResponse(c);

    JS_FreeValue(m_engine->context(), jsClient);   // 释放引用；c 由 GC 回收
    auto it = std::find(m_pending.begin(), m_pending.end(), jsClient);
    if (it != m_pending.end())
        m_pending.erase(it);
}

void JZJSWebApp::timerEvent(JZTimerEvent* event)
{
    int64_t t = nowMs();
    for (size_t i = 0; i < m_pending.size();)
    {
        JSValue jsClient = m_pending[i];
        JZJSWebClient* c = (JZJSWebClient*)JS_GetOpaque(jsClient, m_clientClassId);
        if (c->settled && !c->done)
        {
            // 在某次异步回调中已结算：发送并释放引用（finalizeClient 会 erase，i 不变）
            finalizeClient(jsClient);
        }
        else if (c->done)
        {
            i++;   // 超时僵尸项，已发送过，跳过
        }
        else if (t > c->expireMs)
        {
            // 超时：回 504。保留 c（响应字段仍有效）以免后续迟到的异步回调写入已释放内存，
            // 仅释放 JS 引用并标记 done。
            c->status = 504;
            c->contentType = "text/plain; charset=utf-8";
            c->body = "handler timeout";
            c->settled = true;
            c->done = true;

            sendResponse(c);

            JS_FreeValue(m_engine->context(), jsClient);
            m_pending.erase(m_pending.begin() + i);
        }
        else
        {
            i++;
        }
    }
}
