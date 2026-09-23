#ifndef JS_ENGINE_NET_MODULE_H_
#define JS_ENGINE_NET_MODULE_H_

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "JZJSEngine.h"
#include "JZHttpServer.h"
#include "JZNetworkAccessManager.h"
#include "JZNetworkInterface.h"
#include "JZObjectPointer.h"
#include "JZMqttClient.h"
#include "JZTcpServer.h"
#include "JZTcpSocket.h"
#include "JZUdpSocket.h"
#include "JZUrl.h"
#include "JZWebSocket.h"
#include "JZWebSocketServer.h"
#include "json/json.h"

class JZJSNetModule;

// 原生事件 -> JS 回调。回调以 JSValue 保存(存时 dup，移除/析构时 free)。
class JZJSEventSink
{
public:
    JZJSEventSink(JZJSEngine* engine);
    ~JZJSEventSink();

    // freeValues=false 只解绑不释放(用于引擎销毁路径, 此时 JS 值可能已随 ctx 失效)
    void clear(bool freeValues = true);

    // 把所有回调移出(不释放), 交给调用方统一释放: 释放过程可能连锁触发 JS 对象
    // 析构 -> 反过来修改模块的 sink 表, 所以必须"先解绑收集, 再统一释放"。
    void takeCallbacks(std::vector<std::pair<int, JSValue>>& out);

    // 标记对象正在销毁: 底层 socket 析构时仍会派发事件, 这之后的 emit 全部丢弃,
    // 避免回调访问已经半销毁的包装对象。
    void dispose();
    bool isDisposed() const;

    // 回调要挂到模块的 GC 根数组上, 否则 quickjs 的 GC 会把"只被 C++ 引用计数持有"
    // 的闭包回收掉(JS 对象 <-> C++ 句柄 <-> 回调 形成循环), C++ 侧就成悬垂 JSValue。
    void setModule(JZJSNetModule* module);

    void on(const std::string& event, JSValue cb);
    void off(const std::string& event, JSValue cb);  // cb 为 null 时清空该事件
    bool hasListener(const std::string& event) const;

    // emit 会消费传入的 argv 引用
    void emit(const std::string& event);
    void emit(const std::string& event, JSValue a1);
    void emit(const std::string& event, JSValue a1, JSValue a2);
    void emit(const std::string& event, JSValue a1, JSValue a2, JSValue a3);

protected:
    void emitArgs(const std::string& event, std::vector<JSValue>& argv);

    JZJSEngine* m_engine;
    JZJSNetModule* m_module;
    bool m_disposed;
    // pair = (GC 根数组中的 id, 带引用的回调)
    std::map<std::string, std::vector<std::pair<int, JSValue>>> m_listeners;
};

// TCP socket 包装
struct JZJSNetSocket
{
    JZJSNetSocket(JZJSEngine* engine = nullptr, JZJSNetModule* module = nullptr);
    ~JZJSNetSocket();

    bool write(JSValue data);
    void end();
    void destroy();
    int state() const;          // 0 opening / 1 open / 3 closed
    void bind(JZTcpSocket* socket, const std::string& host, int port);
    void connectTo(int port, const std::string& host);

    // 取底层连接; 连接**已经没了**(accept 出来的会被 server 析构连带释放)返回 nullptr
    // 并顺手摘掉本地指针 —— 之后所有用到 socket 的地方都不再碰它, 避免踩已释放的内存。
    JZTcpSocket* liveSocket();

    JZJSEngine* engine;
    JZJSNetModule* module;
    JZTcpSocket* socket;
    JZJSEventSink sink;
    std::string host;
    int port;
    int64_t bytesRead;
    int64_t bytesWritten;
};

// TCP server 包装
struct JZJSNetServer
{
    JZJSNetServer(JZJSEngine* engine = nullptr, JZJSNetModule* module = nullptr);
    ~JZJSNetServer();

    bool listen(int port, const std::string& host);
    void close();
    Json::Value address() const;

    JZJSEngine* engine;
    JZJSNetModule* module;
    JZTcpServer* server;
    JZJSEventSink sink;
    int listenPort;
    std::string listenHost;
    bool closed;
};

// UDP socket 包装
struct JZJSUdpSocket
{
    JZJSUdpSocket(JZJSEngine* engine = nullptr, JZJSNetModule* module = nullptr);
    ~JZJSUdpSocket();

    bool bind(int port);
    void send(const std::string& ip, int port, JSValue data);
    void close();
    Json::Value address() const;

    JZJSEngine* engine;
    JZJSNetModule* module;
    JZUdpSocket* socket;
    JZJSEventSink sink;
    int bindPort;
    bool closed;
};

// WebSocket 包装(客户端连接与服务端 accept 出来的连接共用)
struct JZJSWebSocket
{
    JZJSWebSocket(JZJSEngine* engine = nullptr, JZJSNetModule* module = nullptr);
    ~JZJSWebSocket();

    void send(JSValue data, bool binary);
    void close();
    void ping();
    int state() const;

    JZJSEngine* engine;
    JZJSNetModule* module;
    // socket 由本包装**独占**: close() 里 disconnectFromHost() + delete(见 .cpp)。
    // accept 出来的那份在接管时已经 setParent(nullptr) 从 JZWebSocketServer 手里摘出来,
    // 所以 server 关闭/析构不会再把它一起 delete(否则 JS 侧还攥着就悬垂了)。
    JZWebSocket* socket;
    JZJSEventSink sink;
    bool everConnected;
    bool closed;
};

// WebSocket server 包装
struct JZJSWebSocketServer
{
    JZJSWebSocketServer(JZJSEngine* engine = nullptr, JZJSNetModule* module = nullptr);
    ~JZJSWebSocketServer();

    bool listen(int port, const std::string& host);
    void close();
    Json::Value address() const;

    JZJSEngine* engine;
    JZJSNetModule* module;
    JZWebSocketServer* server;
    JZJSEventSink sink;
    int listenPort;
    std::string listenHost;
    bool closed;
};

// MQTT client 包装
struct JZJSMqttClient
{
    JZJSMqttClient(JZJSEngine* engine = nullptr, JZJSNetModule* module = nullptr);
    ~JZJSMqttClient();

    void publish(const std::string& topic, JSValue data, int qos, bool retain);
    void subscribe(const std::string& topic, int qos);
    void unsubscribe(const std::string& topic);
    void end();

    JZJSEngine* engine;
    JZJSNetModule* module;
    JZMqttClient* client;
    JZJSEventSink sink;
    bool connected;
};

struct JZJSHttpServer;

// HTTP 请求转发: JZHttpServer 收到请求后交给 JS 侧的 'request' 事件。
// 挂在 catch-all 路由上（"/<path:path>"），全部请求都走这一个 handler。
class JZJSHttpHandler : public JZHttpHandler
{
public:
    JZJSHttpServer* server = nullptr;
    void handle(JZHttpServerClient* client, const JZHttpRequest& req) override;
};

// HTTP 服务端响应包装（每个请求一个）。
// 与 Node 的差别：不是流式写，write() 只缓冲，end() 时一次性发出（见 types/http.d.ts）。
struct JZJSHttpResponse
{
    JZJSHttpResponse(JZJSEngine* engine = nullptr, JZJSNetModule* module = nullptr);
    ~JZJSHttpResponse();

    void writeHead(int status, const Json::Value& headers);
    void setHeader(const std::string& name, const std::string& value);
    void write(JSValue data);
    void end(JSValue data);

    JZJSEngine* engine;
    JZJSNetModule* module;
    JZHttpServerClient* client;   // 不拥有（由 JZHttpServer/JZHttpServerClient 管理）
    JZByteArray body;             // write() 先缓冲，end() 时才发
    Json::Value headers;
    int statusCode;
    bool headWritten;
    bool finished;
};

// HTTP server 包装
struct JZJSHttpServer
{
    JZJSHttpServer(JZJSEngine* engine = nullptr, JZJSNetModule* module = nullptr);
    ~JZJSHttpServer();

    bool listen(int port, const std::string& host);
    void close();
    Json::Value address() const;

    JZJSEngine* engine;
    JZJSNetModule* module;
    JZHttpServer* server;
    JZJSHttpHandler* handler;
    JZJSEventSink sink;
    int listenPort;
    std::string listenHost;
    bool closed;
};

// JS 网络模块: 注册 __jz.* 原子 API + 内嵌 JS 适配层(提供 Node 风格 API)
class JZJSNetModule : public JZObject
{
public:
    JZJSNetModule();
    ~JZJSNetModule();

    void initEngine(JZJSEngine* engine);
    void deinitEngine();

    // ---- 原子 API(由内嵌 JS 适配层调用) ----
    JSValue netCreateSocket();
    JSValue netCreateServer();
    JSValue dgramCreateSocket();
    JSValue wsConnect(const std::string& url);
    JSValue wsCreateServer();
    JSValue mqttConnect(const std::string& url, const Json::Value& options);
    void httpRequest(int promiseHandle, const Json::Value& options, JSValue body);
    JSValue httpCreateServer();
    Json::Value urlParse(const std::string& url);
    Json::Value osNetworkInterfaces();
    JSValue bytesFromString(const std::string& text);
    std::string bytesToString(JSValue data);

    static std::string jsValueToString(JSContext* ctx, JSValue v);
    static bool jsValueToBytes(JSContext* ctx, JSValue v, JZByteArray& out);

    // 事件回调表登记/注销(包装对象析构时会注销自己, 避免 deinitEngine 访问悬垂指针)
    // disposer: 销毁该对象底层资源的回调。引擎销毁前由模块统一调用,
    // 避免把 delete 网络对象这种重活留到 GC 的 finalizer 里做。
    void addSink(JZJSEventSink* sink, std::function<void()> disposer = std::function<void()>());
    void removeSink(JZJSEventSink* sink);

    // JS 回调的 GC 根: 存进根数组使闭包始终可达, 释放时按 id 移除
    int retainCallback(JSValue cb);
    void releaseCallback(int id, JSValue value);

    // 由 JZJSHttpHandler 调用：把响应句柄包成 JS 值
    JSValue makeHttpResponseValue(JZJSHttpResponse* obj);

protected:
    friend struct JZJSNetServer;
    friend struct JZJSWebSocketServer;
    friend struct JZJSWebSocket;
    friend class JZJSHttpHandler;

    JSValue makeSocketValue(JZJSNetSocket* obj);
    JSValue makeWsValue(JZJSWebSocket* obj);
    void onNetFinish();

    // 注册时(initEngine 末尾)把 events/url/ws/http 四个包跑一遍, 让它们把 Web 标准全局
    // (EventEmitter / URL·URLSearchParams / WebSocket / fetch·Headers·Response)挂上 globalThis。
    // 见 .cpp 里的详细说明(为什么必须按契约相对名加载、为什么用 tryRunModuleFile)。
    void loadWebGlobalModules();

    JZJSEngine* m_engine;
    JZNetworkAccessManager* m_net;
    int m_typeSocket;
    int m_typeServer;
    int m_typeUdp;
    int m_typeWs;
    int m_typeWsServer;
    int m_typeMqtt;
    int m_typeHttpServer;
    int m_typeHttpResponse;
    std::map<JZNetworkReply*, int> m_replyMap;
    std::map<JZNetworkReply*, std::string> m_replyUrl;
    std::vector<JZJSEventSink*> m_sinks;
    // 回调 GC 根数组**不由本模块维护**: 统一用 JZJSEngine::retainCallback/releaseCallback
    // (见那里的说明) —— 控件回调槽与网络回调共用同一份根数组。
    std::map<JZJSEventSink*, std::function<void()>> m_disposers;
};

#endif  // !JS_ENGINE_NET_MODULE_H_
