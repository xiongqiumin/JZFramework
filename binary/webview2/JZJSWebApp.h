#pragma once
#include <map>
#include <string>
#include <vector>

#include "JZObject.h"
#include "JZDir.h"
#include "JZHttpServer.h"
#include "JZHttpHandler.h"
#include "JZHttpRoute.h"
#include "JZJSEngine.h"
#include "JZJSHttpModule.h"
#include "JZHttpRequest.h"
#include "JZJSBind.h"

class JZHttpServerClient;
class JZHttpRequest;

// 一个动态请求对应的“客户端桥接”对象（绑定为 JS 的 WebClient 类）。
// 它作为 opaque 挂在 JS 的 client 对象上，由 JS 引擎的 GC 负责 delete，
// 因此 C++ 侧只持有 JSValue 引用，绝不手动 delete 本对象。
//
// 请求数据（只读，JS 通过 client.method / path / ... 读取）：
//   method / path / apiPath / url / body / query / params / headers
// 响应方法（可异步调用，JS 自己决定何时返回）：
//   respond(status, contentType, body) / send(v) / json(v) / text(s) / html(s) / status(code) / set(name, value)
class JZJSWebClient
{
public:
    JZHttpServerClient* client = nullptr;   // 原始 HTTP 客户端（keep-alive 期间有效，不由本对象拥有）
    JZHttpRequest req;                       // 请求快照
    std::string apiPath;                     // /api 之后的子路径

    int status = 200;
    std::string body;
    std::string contentType;
    std::map<std::string, std::string> headers;
    bool settled = false;   // 是否已通过响应方法写入响应
    int64_t expireMs = 0;
    bool done = false;      // HTTP 响应是否已发送（含超时），用于跳过僵尸项

    // ---- 请求只读字段 ----
    std::string method() const;
    std::string path() const;
    std::string apiPathStr() const { return apiPath; }
    std::string url() const;
    std::string bodyStr() const;
    Json::Value query() const;
    Json::Value params() const;
    Json::Value headersObj() const;

    // ---- 响应方法 ----
    void respond(int status, const std::string& contentType, const std::string& body);
    void send(const Json::Value& data);
    void json(const Json::Value& data);
    void text(const std::string& text);
    void html(const std::string& html);
    void setStatus(int code);
    void setHeader(const std::string& name, const std::string& value);
};

// HTTP 动态 API 的入口 handler：server 路由 /api/<path:rest> 指向它
class JZJSWebHandler : public JZHttpHandler
{
public:
    JZJSWebApp* m_app = nullptr;

    void handle(JZHttpServerClient* client, const JZHttpRequest& req) override
    {
        m_app->dispatch(client, req);
    }
};

// JZJSWebApp —— 把 HTTP 动态 API 桥接到 JZJSEngine，构建一个简单的 Node.js 风格环境。
//
//   - JS 中直接定义全局函数 onApiCall(client)，所有 /api/* 请求都会派发给它：
//        onApiCall = function(client) {
//            client.json({ msg: 'hello from js engine' });
//        };
//   - client 是绑定类 WebClient 的实例，暴露请求数据（只读）与响应方法（见 JZJSWebClient）
//   - 支持同步与异步 handler：response 在 client.* 被调用时才发送；未调用则等待（超时 30s 回 504）
//   - 启动时自动加载 <appDir>/scripts 下的所有 .js（按文件名排序）
//   - 同时注入 fetch（JZJSHttpModule），更接近 Node 环境
//
// 注意：JS 引擎的 context 只在创建它的线程上安全。JZWebServer 与 HTTP server 都运行在主线程，
// 动态分发中对 engine 的调用都在主线程，不存在并发问题。
class JZJSWebApp : public JZObject
{
public:
    JZJSWebApp(JZObject* parent = nullptr);
    ~JZJSWebApp();

    // 初始化引擎、注册 WebClient 类与 onApiCall/fetch 全局、加载 scripts 下的脚本。appDir 即 <exeDir>/app
    bool init(const std::string& appDir);
    void shutdown();

    // HTTP 侧入口：把请求包装成 WebClient 对象，调用全局 onApiCall(client)
    void dispatch(JZHttpServerClient* client, const JZHttpRequest& req);

private:
    void registerClientClass();
    void registerGlobals();

    JSValue buildClient(JZHttpServerClient* client, const JZHttpRequest& req);
    void sendResponse(JZJSWebClient* c);
    void finalizeClient(JSValue jsClient);   // 发送 HTTP 响应并释放 JS 引用（c 由 GC 回收）

    virtual void timerEvent(JZTimerEvent* event) override;  // 轮询挂起的请求，发送已结算/超时的响应
    static int64_t nowMs();

    JZJSEngine* m_engine = nullptr;
    JZJSHttpModule* m_http = nullptr;

    int m_clientClassId = 0;                  // WebClient 的 JSClassID，用于从 JSValue 取回 c
    std::vector<JSValue> m_pending;           // 等待响应回写的 client JS 对象（持有 dup 引用）

    static JZJSWebApp* s_instance;   // 供 onApiCall 派发取回 this
};
