#include "JZJSNetModule.h"

#include <cstdio>

#include "JZByteArray.h"
#include "JZJSBind.h"

// 注: 早先这里有个自写的 jsSameValue(直接比 ptr + tag)。现在 off() 里统一用 quickjs 的
// JS_IsStrictEqual —— 语义等价(同一对象 / 同一原始值), 且不必依赖 JSValue 的内部布局。

// ======================================================================
// JZJSEventSink
// ======================================================================
JZJSEventSink::JZJSEventSink(JZJSEngine* engine)
{
    m_engine = engine;
    m_module = nullptr;
    m_disposed = false;
}

void JZJSEventSink::setModule(JZJSNetModule* module)
{
    m_module = module;
}

void JZJSEventSink::dispose()
{
    m_disposed = true;
}

bool JZJSEventSink::isDisposed() const
{
    return m_disposed;
}

JZJSEventSink::~JZJSEventSink()
{
    clear();
}

void JZJSEventSink::clear(bool freeValues)
{
    JSContext* ctx = m_engine ? m_engine->context() : nullptr;
    if (ctx && freeValues)
    {
        for (auto& kv : m_listeners)
        {
            for (size_t i = 0; i < kv.second.size(); i++)
            {
                if (m_module)
                    m_module->releaseCallback(kv.second[i].first, kv.second[i].second);
                else
                    JS_FreeValue(ctx, kv.second[i].second);
            }
        }
    }
    m_listeners.clear();
}

void JZJSEventSink::takeCallbacks(std::vector<std::pair<int, JSValue>>& out)
{
    for (auto& kv : m_listeners)
    {
        for (size_t i = 0; i < kv.second.size(); i++)
            out.push_back(kv.second[i]);
    }
    m_listeners.clear();
}

void JZJSEventSink::on(const std::string& event, JSValue cb)
{
    JSContext* ctx = m_engine->context();
    if (!JS_IsFunction(ctx, cb))
        return;

    int id = m_module ? m_module->retainCallback(cb) : -1;
    m_listeners[event].push_back(std::make_pair(id, JS_DupValue(ctx, cb)));
}

void JZJSEventSink::off(const std::string& event, JSValue cb)
{
    JSContext* ctx = m_engine->context();
    auto it = m_listeners.find(event);
    if (it == m_listeners.end())
        return;

    if (JS_IsNull(cb) || JS_IsUndefined(cb))
    {
        for (size_t i = 0; i < it->second.size(); i++)
        {
            if (m_module)
                m_module->releaseCallback(it->second[i].first, it->second[i].second);
            else
                JS_FreeValue(ctx, it->second[i].second);
        }
        m_listeners.erase(it);
        return;
    }

    auto& list = it->second;
    for (size_t i = 0; i < list.size();)
    {
        if (JS_IsStrictEqual(ctx, list[i].second, cb))
        {
            if (m_module)
                m_module->releaseCallback(list[i].first, list[i].second);
            else
                JS_FreeValue(ctx, list[i].second);
            list.erase(list.begin() + i);
        }
        else
        {
            i++;
        }
    }
    if (list.empty())
        m_listeners.erase(it);
}

bool JZJSEventSink::hasListener(const std::string& event) const
{
    auto it = m_listeners.find(event);
    return it != m_listeners.end() && !it->second.empty();
}

void JZJSEventSink::emitArgs(const std::string& event, std::vector<JSValue>& argv)
{
    JSContext* ctx = m_engine->context();
    auto it = m_listeners.find(event);
    if (m_disposed || it == m_listeners.end() || it->second.empty())
    {
        for (size_t i = 0; i < argv.size(); i++)
            JS_FreeValue(ctx, argv[i]);
        return;
    }

    // 拷贝一份, 允许回调里增删监听
    std::vector<JSValue> list;
    for (size_t i = 0; i < it->second.size(); i++)
        list.push_back(it->second[i].second);
    for (size_t i = 0; i < list.size(); i++)
    {
        JSValue ret = JS_Call(ctx, list[i], JS_UNDEFINED, (int)argv.size(), argv.data());
        if (JS_IsException(ret))
        {
            JSValue exc = JS_GetException(ctx);
            JS_FreeValue(ctx, exc);
        }
        JS_FreeValue(ctx, ret);
    }

    for (size_t i = 0; i < argv.size(); i++)
        JS_FreeValue(ctx, argv[i]);
}

void JZJSEventSink::emit(const std::string& event)
{
    std::vector<JSValue> argv;
    emitArgs(event, argv);
}

void JZJSEventSink::emit(const std::string& event, JSValue a1)
{
    std::vector<JSValue> argv;
    argv.push_back(a1);
    emitArgs(event, argv);
}

void JZJSEventSink::emit(const std::string& event, JSValue a1, JSValue a2)
{
    std::vector<JSValue> argv;
    argv.push_back(a1);
    argv.push_back(a2);
    emitArgs(event, argv);
}

void JZJSEventSink::emit(const std::string& event, JSValue a1, JSValue a2, JSValue a3)
{
    std::vector<JSValue> argv;
    argv.push_back(a1);
    argv.push_back(a2);
    argv.push_back(a3);
    emitArgs(event, argv);
}

// ======================================================================
// 工具
// ======================================================================
static JSValue jsMakeUint8Array(JSContext* ctx, const JZByteArray& data)
{
    return JS_NewUint8ArrayCopy(ctx, (const uint8_t*)data.data(), data.size());
}

std::string JZJSNetModule::jsValueToString(JSContext* ctx, JSValue v)
{
    if (JS_IsString(v))
    {
        size_t len = 0;
        const char* p = JS_ToCStringLen(ctx, &len, v);
        std::string s(p ? p : "", len);
        JS_FreeCString(ctx, p);
        return s;
    }
    return std::string();
}

bool JZJSNetModule::jsValueToBytes(JSContext* ctx, JSValue v, JZByteArray& out)
{
    out.clear();
    if (JS_IsString(v))
    {
        size_t len = 0;
        const char* p = JS_ToCStringLen(ctx, &len, v);
        if (!p)
            return false;
        out = JZByteArray(p, (int)len);
        JS_FreeCString(ctx, p);
        return true;
    }

    size_t size = 0;
    uint8_t* buf = jzUint8ArrayOrNull(ctx, &size, v);
    if (buf)
    {
        out = JZByteArray((const char*)buf, (int)size);
        return true;
    }

    if (JS_IsArrayBuffer(v))
    {
        size_t ab_size = 0;
        uint8_t* ab = JS_GetArrayBuffer(ctx, &ab_size, v);
        if (ab)
        {
            out = JZByteArray((const char*)ab, (int)ab_size);
            return true;
        }
    }
    return false;
}

// ======================================================================
// JZJSNetSocket
// ======================================================================
JZJSNetSocket::JZJSNetSocket(JZJSEngine* engine, JZJSNetModule* module)
    : sink(engine)
{
    this->engine = engine;
    this->module = module;
    sink.setModule(module);
    socket = nullptr;
    owned = true;
    port = 0;
    bytesRead = 0;
    bytesWritten = 0;
    destroyed = false;
}

JZJSNetSocket::~JZJSNetSocket()
{
    destroy();
    if (module)
        module->removeSink(&sink);
}

void JZJSNetSocket::bind(JZTcpSocket* s, const std::string& h, int p)
{
    socket = s;
    host = h;
    port = p;

    JZJSNetSocket* self = this;
    JSContext* ctx = engine->context();

    connect(&s->sigConnected, [self]() { self->sink.emit("connect"); });

    connect(&s->sigReadyRead, [self]() {
        if (!self->socket)
            return;
        JSContext* c = self->engine->context();
        JZByteArray data = self->socket->readAll();
        if (data.isEmpty())
            return;
        self->bytesRead += data.size();
        self->sink.emit("data", jsMakeUint8Array(c, data));
    });

    connect(&s->sigDisconnected, [self]() {
        self->sink.emit("end");
        self->sink.emit("close");
        self->destroyed = true;
    });

    connect(&s->sigError, [self](const std::string& err) {
        JSContext* c = self->engine->context();
        self->sink.emit("error", JS_NewString(c, err.c_str()));
    });

    (void)ctx;
}

void JZJSNetSocket::connectTo(int p, const std::string& h)
{
    if (!socket)
        return;

    host = h.empty() ? std::string("127.0.0.1") : h;
    port = p;
    socket->connectToHost(host, port);
}

bool JZJSNetSocket::write(JSValue data)
{
    if (!socket || !socket->isConnected())
        return false;

    JZByteArray buf;
    if (!JZJSNetModule::jsValueToBytes(engine->context(), data, buf))
        return false;

    socket->write(buf);
    bytesWritten += buf.size();
    return true;
}

void JZJSNetSocket::end()
{
    if (!socket)
        return;
    socket->disconnectFromHost();
}

void JZJSNetSocket::destroy()
{
    sink.dispose();   // 之后底层派发的事件一律丢弃
    if (socket)
    {
        // 先摘指针再删除: 析构期间对象仍可能派发事件, 回调里靠空指针判断跳过
        JZTcpSocket* s = socket;
        socket = nullptr;
        s->close();
        if (owned)   // accept 出来的连接由 server 释放
            delete s;
    }
    destroyed = true;
}

int JZJSNetSocket::state() const
{
    if (!socket || destroyed)
        return 3;
    if (socket->isConnected())
        return 1;
    return 0;
}

// ======================================================================
// JZJSNetServer
// ======================================================================
JZJSNetServer::JZJSNetServer(JZJSEngine* engine, JZJSNetModule* module)
    : sink(engine)
{
    this->engine = engine;
    this->module = module;
    sink.setModule(module);
    server = nullptr;
    listenPort = 0;
    closed = false;
}

JZJSNetServer::~JZJSNetServer()
{
    close();
    if (module)
        module->removeSink(&sink);
}

bool JZJSNetServer::listen(int port, const std::string& host)
{
    if (!server)
        return false;

    bool ok = host.empty() ? server->listen(port) : server->listen(host, port);
    if (ok)
    {
        listenPort = server->port();
        listenHost = host;
        sink.emit("listening");
    }
    return ok;
}

void JZJSNetServer::close()
{
    if (sink.isDisposed())
        return;

    if (!closed)
    {
        closed = true;
        sink.emit("close");
    }

    sink.dispose();
    if (server)
    {
        JZTcpServer* s = server;
        server = nullptr;
        s->close();
        delete s;
    }
}

Json::Value JZJSNetServer::address() const
{
    if (!server || listenPort == 0)
        return Json::Value(Json::nullValue);

    Json::Value res;
    res["port"] = listenPort;
    res["address"] = listenHost.empty() ? std::string("0.0.0.0") : listenHost;
    res["family"] = "IPv4";
    return res;
}

// ======================================================================
// JZJSUdpSocket
// ======================================================================
JZJSUdpSocket::JZJSUdpSocket(JZJSEngine* engine, JZJSNetModule* module)
    : sink(engine)
{
    this->engine = engine;
    this->module = module;
    sink.setModule(module);
    socket = nullptr;
    bindPort = 0;
    closed = false;
}

JZJSUdpSocket::~JZJSUdpSocket()
{
    close();
    if (module)
        module->removeSink(&sink);
}

bool JZJSUdpSocket::bind(int port)
{
    if (!socket)
        return false;

    if (!socket->bind(port))
        return false;

    bindPort = port;
    sink.emit("listening");
    return true;
}

void JZJSUdpSocket::send(const std::string& ip, int port, JSValue data)
{
    if (!socket)
        return;

    JZByteArray buf;
    if (!JZJSNetModule::jsValueToBytes(engine->context(), data, buf))
        return;

    socket->writeDatagram(ip, port, buf);
}

void JZJSUdpSocket::close()
{
    if (sink.isDisposed())
        return;

    if (!closed)
    {
        closed = true;
        sink.emit("close");
    }

    sink.dispose();
    if (socket)
    {
        JZUdpSocket* s = socket;
        socket = nullptr;
        s->close();
        delete s;
    }
}

Json::Value JZJSUdpSocket::address() const
{
    Json::Value res;
    res["address"] = "0.0.0.0";
    res["port"] = bindPort;
    res["family"] = "IPv4";
    return res;
}

// ======================================================================
// JZJSWebSocket
// ======================================================================
JZJSWebSocket::JZJSWebSocket(JZJSEngine* engine, JZJSNetModule* module)
    : sink(engine)
{
    this->engine = engine;
    this->module = module;
    sink.setModule(module);
    socket = nullptr;
    owned = true;
    everConnected = false;
    closed = false;
}

JZJSWebSocket::~JZJSWebSocket()
{
    close();
    if (module)
        module->removeSink(&sink);
}

void JZJSWebSocket::send(JSValue data, bool binary)
{
    if (!socket || !socket->isConnected())
        return;

    JZByteArray buf;
    if (!JZJSNetModule::jsValueToBytes(engine->context(), data, buf))
        return;

    if (binary)
        socket->sendBinaryMessage(buf);
    else
        socket->sendTextMessage(std::string(buf.data(), buf.size()));
}

void JZJSWebSocket::close()
{
    if (sink.isDisposed())
        return;

    if (!closed)
    {
        closed = true;
        sink.emit("close", JS_NewInt32(engine->context(), 1000), JS_NewString(engine->context(), ""));
    }

    sink.dispose();
    if (socket)
    {
        JZWebSocket* s = socket;
        socket = nullptr;
        s->disconnectFromHost();
        if (owned)   // accept 出来的连接由 server 释放
            delete s;
    }
}

void JZJSWebSocket::ping()
{
    if (socket)
        socket->sendPing();
}

int JZJSWebSocket::state() const
{
    if (!socket)
        return 3;
    if (socket->isConnected())
        return 1;
    return everConnected ? 3 : 0;
}

// ======================================================================
// JZJSWebSocketServer
// ======================================================================
JZJSWebSocketServer::JZJSWebSocketServer(JZJSEngine* engine, JZJSNetModule* module)
    : sink(engine)
{
    this->engine = engine;
    this->module = module;
    sink.setModule(module);
    server = nullptr;
    listenPort = 0;
    closed = false;
}

JZJSWebSocketServer::~JZJSWebSocketServer()
{
    close();
    if (module)
        module->removeSink(&sink);
}

bool JZJSWebSocketServer::listen(int port, const std::string& host)
{
    if (!server)
        return false;

    bool ok = host.empty() ? server->listen(port) : server->listen(host, port);
    if (ok)
    {
        listenPort = port;
        listenHost = host;
        sink.emit("listening");
    }
    return ok;
}

void JZJSWebSocketServer::close()
{
    if (sink.isDisposed())
        return;

    if (!closed)
    {
        closed = true;
        sink.emit("close");
    }

    sink.dispose();
    if (server)
    {
        JZWebSocketServer* s = server;
        server = nullptr;
        s->close();
        delete s;
    }
}

Json::Value JZJSWebSocketServer::address() const
{
    if (!server || listenPort == 0)
        return Json::Value(Json::nullValue);

    Json::Value res;
    res["port"] = listenPort;
    res["address"] = listenHost.empty() ? std::string("0.0.0.0") : listenHost;
    res["family"] = "IPv4";
    return res;
}

// ======================================================================
// JZJSMqttClient
// ======================================================================
JZJSMqttClient::JZJSMqttClient(JZJSEngine* engine, JZJSNetModule* module)
    : sink(engine)
{
    this->engine = engine;
    this->module = module;
    sink.setModule(module);
    client = nullptr;
    connected = false;
}

JZJSMqttClient::~JZJSMqttClient()
{
    end();
    if (module)
        module->removeSink(&sink);
}

void JZJSMqttClient::publish(const std::string& topic, JSValue data, int qos, bool retain)
{
    if (!client)
        return;

    JZByteArray buf;
    if (!JZJSNetModule::jsValueToBytes(engine->context(), data, buf))
        return;

    JZMqttPublishOpt opt;
    opt.topic = topic;
    opt.payload = buf;
    opt.qos = (uint8_t)qos;
    opt.retain = retain;
    client->publish(opt);
}

void JZJSMqttClient::subscribe(const std::string& topic, int qos)
{
    if (!client)
        return;

    JZMqttSubOpt opt;
    opt.topic = topic;
    opt.qos = (uint8_t)qos;
    client->subscribe(opt);
}

void JZJSMqttClient::unsubscribe(const std::string& topic)
{
    if (!client)
        return;

    JZMqttSubOpt opt;
    opt.topic = topic;
    client->unsubscribe(opt);
}

void JZJSMqttClient::end()
{
    if (sink.isDisposed())
        return;

    if (connected)
    {
        connected = false;
        sink.emit("close");
    }

    sink.dispose();
    if (client)
    {
        JZMqttClient* c = client;
        client = nullptr;
        c->disconnectFromHost();
        delete c;
    }
}

// ======================================================================
// JZJSHttpResponse
// ======================================================================
JZJSHttpResponse::JZJSHttpResponse(JZJSEngine* engine, JZJSNetModule* module)
{
    this->engine = engine;
    this->module = module;
    client = nullptr;
    statusCode = 200;
    headWritten = false;
    finished = false;
}

JZJSHttpResponse::~JZJSHttpResponse()
{
    // 兜底：JS 侧忘了 end() 也别把连接一直挂着，按已缓冲内容(或空 200)收尾
    if (!finished && client)
        end(JS_UNDEFINED);
}

void JZJSHttpResponse::setHeader(const std::string& name, const std::string& value)
{
    headers[name] = value;
}

void JZJSHttpResponse::writeHead(int status, const Json::Value& hs)
{
    statusCode = status;
    if (!hs.isObject())
        return;
    for (auto it = hs.begin(); it != hs.end(); ++it)
        headers[it.key().asString()] = it->asString();
}

void JZJSHttpResponse::write(JSValue data)
{
    JSContext* ctx = engine ? engine->context() : nullptr;
    if (!ctx)
        return;

    if (!finished)
    {
        JZByteArray buf;
        if (JZJSNetModule::jsValueToBytes(ctx, data, buf))
            body.append(buf);
    }
    // 注意: data 是绑定层传进来的**借用**引用(fromJSValue<JSValue> 直接返回 argv[i], 不 dup),
    // 不能在这里 JS_FreeValue —— 释放的是调用方(JS 侧)那一份, 引用计数被多减一次 => UAF
    // (表现: res.end() 发完响应后 JS 引擎崩/卡, 退出码 0xC0000005)。对齐 JZJSNetSocket::write。
}

void JZJSHttpResponse::end(JSValue data)
{
    JSContext* ctx = engine ? engine->context() : nullptr;
    if (!ctx)
        return;

    if (!finished && !JS_IsUndefined(data))
    {
        JZByteArray buf;
        if (JZJSNetModule::jsValueToBytes(ctx, data, buf))
            body.append(buf);
    }
    // 同 write(): data 是借用引用, 不能 free(见上)。

    if (finished || !client)
    {
        finished = true;
        return;
    }
    finished = true;

    JZHttpRespone resp;
    resp.setStatusCode(statusCode);
    for (auto it = headers.begin(); it != headers.end(); ++it)
        resp.setHeader(it.key().asString(), it->asString());
    resp.setBody(body);

    JZHttpServerClient* c = client;
    client = nullptr;
    c->response(resp.toByteArray());
}

// ======================================================================
// JZJSHttpServer / JZJSHttpHandler
// ======================================================================
JZJSHttpServer::JZJSHttpServer(JZJSEngine* engine, JZJSNetModule* module)
    : sink(engine)
{
    this->engine = engine;
    this->module = module;
    sink.setModule(module);
    server = nullptr;
    handler = nullptr;
    listenPort = 0;
    closed = false;
}

JZJSHttpServer::~JZJSHttpServer()
{
    close();
    if (module)
        module->removeSink(&sink);
}

bool JZJSHttpServer::listen(int port, const std::string& host)
{
    if (!server)
        return false;

    bool ok = host.empty() ? server->listen(port) : server->listen(host, port);
    if (ok)
    {
        listenPort = port;
        listenHost = host;
        sink.emit("listening");
    }
    else
    {
        sink.emit("error", JS_NewString(engine->context(), "listen failed"));
    }
    return ok;
}

void JZJSHttpServer::close()
{
    if (sink.isDisposed())
        return;

    if (!closed)
    {
        closed = true;
        sink.emit("close");
    }

    sink.dispose();
    if (handler)
    {
        JZJSHttpHandler* h = handler;
        handler = nullptr;
        delete h;
    }
    if (server)
    {
        JZHttpServer* s = server;
        server = nullptr;
        s->close();
        delete s;
    }
}

Json::Value JZJSHttpServer::address() const
{
    if (!server || listenPort == 0)
        return Json::Value(Json::nullValue);

    Json::Value res;
    res["port"] = listenPort;
    res["address"] = listenHost.empty() ? std::string("0.0.0.0") : listenHost;
    res["family"] = "IPv4";
    return res;
}

void JZJSHttpHandler::handle(JZHttpServerClient* client, const JZHttpRequest& req)
{
    if (!server || !server->module || !server->engine)
        return;
    if (server->closed || server->sink.isDisposed())
        return;

    JSContext* ctx = server->engine->context();

    JSValue reqObj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, reqObj, "method",
                      JS_NewString(ctx, req.methodString().c_str()));
    std::string url = req.path();
    std::string query = req.query();
    if (!query.empty())
        url += "?" + query;
    JS_SetPropertyStr(ctx, reqObj, "url", JS_NewString(ctx, url.c_str()));
    JS_SetPropertyStr(ctx, reqObj, "httpVersion", JS_NewString(ctx, "1.1"));
    JS_SetPropertyStr(ctx, reqObj, "remoteAddress",
                      JS_NewString(ctx, req.remoteAddr().c_str()));

    JSValue headers = JS_NewObject(ctx);
    const auto& hs = req.headers();
    for (auto it = hs.begin(); it != hs.end(); ++it)
        JS_SetPropertyStr(ctx, headers, it->first.c_str(),
                          JS_NewString(ctx, it->second.c_str()));
    JS_SetPropertyStr(ctx, reqObj, "headers", headers);
    JS_SetPropertyStr(ctx, reqObj, "body", jsMakeUint8Array(ctx, req.body()));

    JZJSHttpResponse* res = new JZJSHttpResponse(server->engine, server->module);
    res->client = client;

    server->sink.emit("request", reqObj, server->module->makeHttpResponseValue(res));
}

// ======================================================================
// JZJSNetModule
// ======================================================================
JZJSNetModule::JZJSNetModule()
{
    m_engine = nullptr;
    m_net = nullptr;
    m_typeSocket = -1;
    m_typeServer = -1;
    m_typeUdp = -1;
    m_typeWs = -1;
    m_typeWsServer = -1;
    m_typeMqtt = -1;
}

JZJSNetModule::~JZJSNetModule()
{
    deinitEngine();
}

void JZJSNetModule::addSink(JZJSEventSink* sink, std::function<void()> disposer)
{
    m_sinks.push_back(sink);
    if (disposer)
        m_disposers[sink] = disposer;
}

int JZJSNetModule::retainCallback(JSValue cb)
{
    // 回调 GC 根由**引擎**统一维护(见 JZJSEngine::retainCallback) —— 控件回调槽用的是同一份,
    // 免得每个模块各挂一个全局根数组、各管一套 id。
    return m_engine ? m_engine->retainCallback(cb) : -1;
}

void JZJSNetModule::releaseCallback(int id, JSValue value)
{
    // 引擎已经走了(ctx 都放掉了): 这份值随 ctx 一起没了, 不能再碰。
    if (!m_engine)
        return;
    m_engine->releaseCallback(id, value);
}

void JZJSNetModule::removeSink(JZJSEventSink* sink)
{
    for (size_t i = 0; i < m_sinks.size(); i++)
    {
        if (m_sinks[i] == sink)
        {
            m_sinks.erase(m_sinks.begin() + i);
            m_disposers.erase(sink);
            return;
        }
    }
}

void JZJSNetModule::initEngine(JZJSEngine* engine)
{
    m_engine = engine;
    if (!m_net)
        m_net = new JZNetworkAccessManager(this);

    auto global = m_engine->globalObject();

    // 注: JS 回调的 GC 根数组(__jzCallbackRoot)现在由**引擎**建(见 JZJSEngine::initJSEnv),
    // 本模块的 retainCallback/releaseCallback 只是转发过去 —— 控件回调槽与网络回调共用一份。
    (void)global;

    // ---- TCP socket ----
    jsbind::ClassBind<JZJSNetSocket> bind_socket("JZJSNetSocket");
    bind_socket.def("connectTo", [](JZJSNetSocket* o, int port, const std::string& host) { o->connectTo(port, host); });
    bind_socket.def("write", [](JZJSNetSocket* o, JSValue data) -> bool { return o->write(data); });
    bind_socket.def("end", [](JZJSNetSocket* o) { o->end(); });
    bind_socket.def("destroy", [](JZJSNetSocket* o) { o->destroy(); });
    bind_socket.def("on", [](JZJSNetSocket* o, const std::string& e, JSValue cb) { o->sink.on(e, cb); });
    bind_socket.def("off", [](JZJSNetSocket* o, const std::string& e, JSValue cb) { o->sink.off(e, cb); });
    bind_socket.defProperty<std::string>("remoteAddress",
                                         [](JZJSNetSocket* o) { return o->host; }, nullptr);
    bind_socket.defProperty<int>("remotePort", [](JZJSNetSocket* o) { return o->port; }, nullptr);
    bind_socket.defProperty<std::string>("localAddress",
                                         [](JZJSNetSocket* o) { return std::string(); }, nullptr);
    bind_socket.defProperty<int>("localPort", [](JZJSNetSocket* o) { return 0; }, nullptr);
    bind_socket.defProperty<int>("bytesRead", [](JZJSNetSocket* o) { return (int)o->bytesRead; }, nullptr);
    bind_socket.defProperty<int>("bytesWritten", [](JZJSNetSocket* o) { return (int)o->bytesWritten; }, nullptr);
    bind_socket.defProperty<bool>("destroyed", [](JZJSNetSocket* o) { return o->destroyed; }, nullptr);
    bind_socket.defProperty<int>("state", [](JZJSNetSocket* o) { return o->state(); }, nullptr);
    
    m_engine->registClass(bind_socket.classDefine());
    m_typeSocket = m_engine->ctypeidToType(typeid(JZJSNetSocket).name());
    
    // ---- TCP server ----
    jsbind::ClassBind<JZJSNetServer> bind_server("JZJSNetServer");
    bind_server.def("listen", [](JZJSNetServer* o, int port, const std::string& host) { o->listen(port, host); });
    bind_server.def("close", [](JZJSNetServer* o) { o->close(); });
    bind_server.def("on", [](JZJSNetServer* o, const std::string& e, JSValue cb) { o->sink.on(e, cb); });
    bind_server.def("off", [](JZJSNetServer* o, const std::string& e, JSValue cb) { o->sink.off(e, cb); });
    bind_server.defProperty<Json::Value>("address", [](JZJSNetServer* o) { return o->address(); }, nullptr);
    bind_server.defProperty<bool>("listening",
                                  [](JZJSNetServer* o) { return o->server && o->server->isListening(); }, nullptr);
    m_engine->registClass(bind_server.classDefine());
    m_typeServer = m_engine->ctypeidToType(typeid(JZJSNetServer).name());

    // ---- UDP ----
    jsbind::ClassBind<JZJSUdpSocket> bind_udp("JZJSUdpSocket");
    bind_udp.def("bind", [](JZJSUdpSocket* o, int port) { o->bind(port); });
    bind_udp.def("send", [](JZJSUdpSocket* o, const std::string& ip, int port, JSValue data) {
        o->send(ip, port, data);
    });
    bind_udp.def("close", [](JZJSUdpSocket* o) { o->close(); });
    bind_udp.def("on", [](JZJSUdpSocket* o, const std::string& e, JSValue cb) { o->sink.on(e, cb); });
    bind_udp.def("off", [](JZJSUdpSocket* o, const std::string& e, JSValue cb) { o->sink.off(e, cb); });
    bind_udp.defProperty<Json::Value>("address", [](JZJSUdpSocket* o) { return o->address(); }, nullptr);
    m_engine->registClass(bind_udp.classDefine());
    m_typeUdp = m_engine->ctypeidToType(typeid(JZJSUdpSocket).name());

    // ---- WebSocket ----
    jsbind::ClassBind<JZJSWebSocket> bind_ws("JZJSWebSocket");
    bind_ws.def("send", [](JZJSWebSocket* o, JSValue data, bool binary) { o->send(data, binary); });
    bind_ws.def("close", [](JZJSWebSocket* o) { o->close(); });
    bind_ws.def("ping", [](JZJSWebSocket* o) { o->ping(); });
    bind_ws.def("on", [](JZJSWebSocket* o, const std::string& e, JSValue cb) { o->sink.on(e, cb); });
    bind_ws.def("off", [](JZJSWebSocket* o, const std::string& e, JSValue cb) { o->sink.off(e, cb); });
    bind_ws.defProperty<int>("state", [](JZJSWebSocket* o) { return o->state(); }, nullptr);
    bind_ws.defProperty<std::string>("url", [](JZJSWebSocket* o) { return std::string(); }, nullptr);
    m_engine->registClass(bind_ws.classDefine());
    m_typeWs = m_engine->ctypeidToType(typeid(JZJSWebSocket).name());

    // ---- WebSocket server ----
    jsbind::ClassBind<JZJSWebSocketServer> bind_wss("JZJSWebSocketServer");
    bind_wss.def("listen", [](JZJSWebSocketServer* o, int port, const std::string& host) { o->listen(port, host); });
    bind_wss.def("close", [](JZJSWebSocketServer* o) { o->close(); });
    bind_wss.def("on", [](JZJSWebSocketServer* o, const std::string& e, JSValue cb) { o->sink.on(e, cb); });
    bind_wss.def("off", [](JZJSWebSocketServer* o, const std::string& e, JSValue cb) { o->sink.off(e, cb); });
    bind_wss.defProperty<Json::Value>("address", [](JZJSWebSocketServer* o) { return o->address(); }, nullptr);
    m_engine->registClass(bind_wss.classDefine());
    m_typeWsServer = m_engine->ctypeidToType(typeid(JZJSWebSocketServer).name());

    // ---- MQTT ----
    jsbind::ClassBind<JZJSMqttClient> bind_mqtt("JZJSMqttClient");
    bind_mqtt.def("publish", [](JZJSMqttClient* o, const std::string& topic, JSValue data, int qos, bool retain) {
        o->publish(topic, data, qos, retain);
    });
    bind_mqtt.def("subscribe", [](JZJSMqttClient* o, const std::string& topic, int qos) { o->subscribe(topic, qos); });
    bind_mqtt.def("unsubscribe", [](JZJSMqttClient* o, const std::string& topic) { o->unsubscribe(topic); });
    bind_mqtt.def("end", [](JZJSMqttClient* o) { o->end(); });
    bind_mqtt.def("on", [](JZJSMqttClient* o, const std::string& e, JSValue cb) { o->sink.on(e, cb); });
    bind_mqtt.def("off", [](JZJSMqttClient* o, const std::string& e, JSValue cb) { o->sink.off(e, cb); });
    bind_mqtt.defProperty<bool>("connected", [](JZJSMqttClient* o) { return o->connected; }, nullptr);
    
    m_engine->registClass(bind_mqtt.classDefine());
    m_typeMqtt = m_engine->ctypeidToType(typeid(JZJSMqttClient).name());

    // ---- HTTP server ----
    jsbind::ClassBind<JZJSHttpServer> bind_http_server("JZJSHttpServer");
    bind_http_server.def("listen", [](JZJSHttpServer* o, int port, const std::string& host) {
        o->listen(port, host);
    });
    bind_http_server.def("close", [](JZJSHttpServer* o) { o->close(); });
    bind_http_server.def("on", [](JZJSHttpServer* o, const std::string& e, JSValue cb) { o->sink.on(e, cb); });
    bind_http_server.def("off", [](JZJSHttpServer* o, const std::string& e, JSValue cb) { o->sink.off(e, cb); });
    bind_http_server.defProperty<Json::Value>("address", [](JZJSHttpServer* o) { return o->address(); }, nullptr);
    bind_http_server.defProperty<bool>("listening",
        [](JZJSHttpServer* o) { return o->server && o->server->isListening(); }, nullptr);
    m_engine->registClass(bind_http_server.classDefine());
    m_typeHttpServer = m_engine->ctypeidToType(typeid(JZJSHttpServer).name());

    // ---- HTTP 服务端响应 ----
    jsbind::ClassBind<JZJSHttpResponse> bind_http_res("JZJSHttpResponse");
    bind_http_res.def("writeHead", [](JZJSHttpResponse* o, int status, const Json::Value& hs) {
        o->writeHead(status, hs);
    });
    bind_http_res.def("setHeader", [](JZJSHttpResponse* o, const std::string& n, const std::string& v) {
        o->setHeader(n, v);
    });
    bind_http_res.def("write", [](JZJSHttpResponse* o, JSValue data) { o->write(data); });
    bind_http_res.def("end", [](JZJSHttpResponse* o, JSValue data) { o->end(data); });
    bind_http_res.defProperty<int>("statusCode", [](JZJSHttpResponse* o) { return o->statusCode; },
                                   [](JZJSHttpResponse* o, int v) { o->statusCode = v; });
    bind_http_res.defProperty<bool>("finished", [](JZJSHttpResponse* o) { return o->finished; }, nullptr);
    m_engine->registClass(bind_http_res.classDefine());
    m_typeHttpResponse = m_engine->ctypeidToType(typeid(JZJSHttpResponse).name());

    // ---- __jz 命名空间 ----
    JSValue jz = m_engine->createObject();

    JSValue ns_net = m_engine->createObject();
    m_engine->registFunction(ns_net, "createSocket",
                             jsbind::createSelfFunction(this, &JZJSNetModule::netCreateSocket));
    m_engine->registFunction(ns_net, "createServer",
                             jsbind::createSelfFunction(this, &JZJSNetModule::netCreateServer));
    m_engine->setProperty(jz, "net", ns_net);

    JSValue ns_dgram = m_engine->createObject();
    m_engine->registFunction(ns_dgram, "createSocket",
                             jsbind::createSelfFunction(this, &JZJSNetModule::dgramCreateSocket));
    m_engine->setProperty(jz, "dgram", ns_dgram);

    JSValue ns_http = m_engine->createObject();
    m_engine->registAsyncFunction(ns_http, "request",
                                  jsbind::createSelfFunction(this, &JZJSNetModule::httpRequest));
    m_engine->registFunction(ns_http, "createServer",
                             jsbind::createSelfFunction(this, &JZJSNetModule::httpCreateServer));
    m_engine->setProperty(jz, "http", ns_http);

    JSValue ns_ws = m_engine->createObject();
    m_engine->registFunction(ns_ws, "connect", jsbind::createSelfFunction(this, &JZJSNetModule::wsConnect));
    m_engine->registFunction(ns_ws, "createServer",
                             jsbind::createSelfFunction(this, &JZJSNetModule::wsCreateServer));
    m_engine->setProperty(jz, "ws", ns_ws);

    JSValue ns_mqtt = m_engine->createObject();
    m_engine->registFunction(ns_mqtt, "connect", jsbind::createSelfFunction(this, &JZJSNetModule::mqttConnect));
    m_engine->setProperty(jz, "mqtt", ns_mqtt);

    JSValue ns_url = m_engine->createObject();
    m_engine->registFunction(ns_url, "parse", jsbind::createSelfFunction(this, &JZJSNetModule::urlParse));
    m_engine->setProperty(jz, "url", ns_url);

    JSValue ns_os = m_engine->createObject();
    m_engine->registFunction(ns_os, "networkInterfaces",
                             jsbind::createSelfFunction(this, &JZJSNetModule::osNetworkInterfaces));
    m_engine->setProperty(jz, "os", ns_os);

    JSValue ns_bytes = m_engine->createObject();
    m_engine->registFunction(ns_bytes, "fromString",
                             jsbind::createSelfFunction(this, &JZJSNetModule::bytesFromString));
    m_engine->registFunction(ns_bytes, "toString",
                             jsbind::createSelfFunction(this, &JZJSNetModule::bytesToString));
    m_engine->setProperty(jz, "bytes", ns_bytes);

    m_engine->setProperty(global, "__jz", jz);
}

void JZJSNetModule::deinitEngine()
{
    // 1) 先主动销毁底层网络对象(在非 GC 上下文里做, finalizer 里 delete 网络对象很危险)
    std::vector<std::function<void()>> disposers;
    for (size_t i = 0; i < m_sinks.size(); i++)
    {
        auto it = m_disposers.find(m_sinks[i]);
        if (it != m_disposers.end())
            disposers.push_back(it->second);
    }
    m_disposers.clear();
    // 逆序: 先关连接, 再关 server(server 析构会连带释放它 accept 出的连接)
    for (int i = (int)disposers.size() - 1; i >= 0; i--)
    {
        disposers[i]();
    }

    // 2) 把所有回调解绑收集出来(表同时清空), 再统一释放:
    // 释放过程可能连锁触发 JS 对象析构 -> 进而回调 removeSink 修改本表,
    // 边遍历边释放会让索引/迭代器失效。
    std::vector<std::pair<int, JSValue>> pending;
    std::vector<JZJSEventSink*> sinks = m_sinks;
    m_sinks.clear();
    for (size_t i = 0; i < sinks.size(); i++)
        sinks[i]->takeCallbacks(pending);

    // 逐个交回引擎释放(置空回调 GC 根对应项 + free)。引擎已走时 ctx 为空, 这些值随 ctx 一起没了。
    JSContext* ctx = m_engine ? m_engine->context() : nullptr;
    if (ctx)
    {
        for (size_t i = 0; i < pending.size(); i++)
            m_engine->releaseCallback(pending[i].first, pending[i].second);
    }

    // 主动跑一轮 GC: 让已经不可达的 JS 包装对象在 ctx 还有效时析构,
    // 否则它们的 finalizer 会在 JS_FreeContext 之后才跑, 用到已释放的 ctx。
    if (ctx)
        JS_RunGC(JS_GetRuntime(ctx));

    auto reply_map = m_replyMap;
    for (auto& pair : reply_map)
    {
        pair.first->close();
        pair.first->deleteLater();
    }
    m_replyMap.clear();
    m_replyUrl.clear();

    // 回调 GC 根数组由引擎自己释放(见 JZJSEngine::deinitJSEnv) —— 本模块不再持有它。
    m_engine = nullptr;
}

JSValue JZJSNetModule::makeSocketValue(JZJSNetSocket* obj)
{
    addSink(&obj->sink, [obj]() { obj->destroy(); });
    return m_engine->createValue(m_typeSocket, obj);
}

JSValue JZJSNetModule::makeWsValue(JZJSWebSocket* obj)
{
    addSink(&obj->sink, [obj]() { obj->close(); });
    return m_engine->createValue(m_typeWs, obj);
}

JSValue JZJSNetModule::netCreateSocket()
{
    JZJSNetSocket* obj = new JZJSNetSocket(m_engine, this);
    obj->bind(new JZTcpSocket(), std::string(), 0);
    return makeSocketValue(obj);
}

JSValue JZJSNetModule::netCreateServer()
{
    JZJSNetServer* obj = new JZJSNetServer(m_engine, this);
    obj->server = new JZTcpServer();
    addSink(&obj->sink, [obj]() { obj->close(); });

    JZJSNetServer* self = obj;
    connect(&obj->server->sigAccept, [this, self]() {
        while (self->server)
        {
            JZTcpSocket* conn = self->server->nextPendingConnection();
            if (!conn)
                break;

            JZJSNetSocket* sock = new JZJSNetSocket(m_engine, this);
            sock->owned = false;   // conn 的 parent 是 server, 不能由我们 delete
            sock->bind(conn, std::string(), self->listenPort);
            self->sink.emit("connection", makeSocketValue(sock));
        }
    });

    return m_engine->createValue(m_typeServer, obj);
}

JSValue JZJSNetModule::dgramCreateSocket()
{
    JZJSUdpSocket* obj = new JZJSUdpSocket(m_engine, this);
    obj->socket = new JZUdpSocket();
    addSink(&obj->sink, [obj]() { obj->close(); });

    JZJSUdpSocket* self = obj;
    connect(&obj->socket->sigReadyRead, [self]() {
        while (self->socket && self->socket->hasPendingDatagrams())
        {
            JSContext* ctx = self->engine->context();
            JZUdpDatagram d = self->socket->readDatagram();
            if (d.data.isEmpty())
                break;

            JSValue rinfo = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, rinfo, "address", JS_NewString(ctx, d.peerIp.c_str()));
            JS_SetPropertyStr(ctx, rinfo, "port", JS_NewInt32(ctx, d.peerPort));
            JS_SetPropertyStr(ctx, rinfo, "family", JS_NewString(ctx, "IPv4"));
            JS_SetPropertyStr(ctx, rinfo, "size", JS_NewInt32(ctx, d.data.size()));

            self->sink.emit("message", jsMakeUint8Array(ctx, d.data), rinfo);
        }
    });

    connect(&obj->socket->sigError, [self](const std::string& err) {
        JSContext* ctx = self->engine->context();
        self->sink.emit("error", JS_NewString(ctx, err.c_str()));
    });

    return m_engine->createValue(m_typeUdp, obj);
}

JSValue JZJSNetModule::wsConnect(const std::string& url)
{
    JZJSWebSocket* obj = new JZJSWebSocket(m_engine, this);
    obj->socket = new JZWebSocket();

    JZJSWebSocket* self = obj;
    JSContext* ctx = m_engine->context();

    connect(&obj->socket->sigConnected, [self]() {
        self->everConnected = true;
        self->sink.emit("open");
    });

    connect(&obj->socket->sigTextMessageReceived, [self](const std::string& msg) {
        JSContext* c = self->engine->context();
        self->sink.emit("message", JS_NewString(c, msg.c_str()));
    });

    connect(&obj->socket->sigBinaryMessageReceived, [self](const JZByteArray& msg) {
        JSContext* c = self->engine->context();
        self->sink.emit("message", jsMakeUint8Array(c, msg));
    });

    connect(&obj->socket->sigDisconnected, [self]() {
        JSContext* c = self->engine->context();
        self->sink.emit("close", JS_NewInt32(c, 1006), JS_NewString(c, ""));
    });

    // JZWebSocket::sigError 是无参信号(不带错误文本)
    connect(&obj->socket->sigError, [self]() {
        JSContext* c = self->engine->context();
        self->sink.emit("error", JS_NewString(c, "websocket error"));
    });

    obj->socket->connectToHost(url);

    (void)ctx;
    return makeWsValue(obj);
}

JSValue JZJSNetModule::wsCreateServer()
{
    JZJSWebSocketServer* obj = new JZJSWebSocketServer(m_engine, this);
    obj->server = new JZWebSocketServer();
    addSink(&obj->sink, [obj]() { obj->close(); });

    JZJSWebSocketServer* self = obj;
    connect(&obj->server->sigAccept, [this, self]() {
        while (self->server)
        {
            JZWebSocket* conn = self->server->nextPendingConnection();
            if (!conn)
                break;

            JZJSWebSocket* ws = new JZJSWebSocket(m_engine, this);
            ws->socket = conn;
            ws->owned = false;   // conn 的 parent 是 server, 不能由我们 delete
            ws->everConnected = true;

            JZJSWebSocket* wself = ws;
            connect(&conn->sigTextMessageReceived, [wself](const std::string& msg) {
                JSContext* c = wself->engine->context();
                wself->sink.emit("message", JS_NewString(c, msg.c_str()));
            });
            connect(&conn->sigBinaryMessageReceived, [wself](const JZByteArray& msg) {
                JSContext* c = wself->engine->context();
                wself->sink.emit("message", jsMakeUint8Array(c, msg));
            });
            connect(&conn->sigDisconnected, [wself]() {
                JSContext* c = wself->engine->context();
                wself->sink.emit("close", JS_NewInt32(c, 1006), JS_NewString(c, ""));
            });

            self->sink.emit("connection", makeWsValue(ws));
        }
    });

    return m_engine->createValue(m_typeWsServer, obj);
}

JSValue JZJSNetModule::mqttConnect(const std::string& url, const Json::Value& options)
{
    JZJSMqttClient* obj = new JZJSMqttClient(m_engine, this);
    obj->client = new JZMqttClient();
    addSink(&obj->sink, [obj]() { obj->end(); });

    JZJSMqttClient* self = obj;
    connect(&obj->client->sigConnected, [self]() {
        self->connected = true;
        self->sink.emit("connect");
    });

    connect(&obj->client->sigMessage, [self](const JZMqttMessage& msg) {
        JSContext* ctx = self->engine->context();
        JSValue topic = JS_NewString(ctx, msg.topic.c_str());
        JSValue payload = jsMakeUint8Array(ctx, msg.payload);
        JSValue qos = JS_NewInt32(ctx, msg.qos);
        self->sink.emit("message", topic, payload, qos);
    });

    connect(&obj->client->sigDisconnected, [self]() {
        bool was = self->connected;
        self->connected = false;
        if (was)
            self->sink.emit("close");
    });

    JZMqttConnectOpt opt;
    opt.clientId = options["clientId"].asString();
    opt.userName = options["userName"].asString();
    opt.password = options["password"].asString();
    if (options["keepalive"].isNumeric())
        opt.keepAlive = (uint16_t)options["keepalive"].asInt();
    opt.cleanSession = options["clean"].asBool();
    if (options["protocolVersion"].isNumeric())
        opt.protoVer = (uint8_t)options["protocolVersion"].asInt();

    if (options["will"].isObject())
    {
        auto& will = options["will"];
        opt.willTopic = will["topic"].asString();
        opt.willMsg = will["payload"].asString();
        opt.willQos = (uint8_t)will["qos"].asInt();
        opt.willRetain = will["retain"].asBool();
    }

    obj->client->connectToHost(url, opt);
    return m_engine->createValue(m_typeMqtt, obj);
}

void JZJSNetModule::httpRequest(int promiseHandle, const Json::Value& options, JSValue body)
{
    std::string url = options["url"].asString();
    if (url.empty())
    {
        m_engine->setPromiseReject(promiseHandle, "http request: url required");
        return;
    }

    std::string method = options["method"].asString();
    if (method.empty())
        method = "GET";

    if (method != "GET" && method != "POST")
    {
        m_engine->setPromiseReject(promiseHandle, "unsupported method: " + method);
        return;
    }

    JZNetworkRequest req;
    req.setUrl(JZUrl(url));
    if (options["headers"].isObject())
    {
        auto& headers = options["headers"];
        auto it = headers.begin();
        while (it != headers.end())
        {
            req.setRawHeader(it.key().asString(), (*it).asString());
            ++it;
        }
    }

    JZByteArray body_bytes;
    jsValueToBytes(m_engine->context(), body, body_bytes);

    JZNetworkReply* reply = nullptr;
    if (method == "GET")
        reply = m_net->get(req);
    else
        reply = m_net->post(req, body_bytes);

    if (options["timeout"].isNumeric())
    {
        int ms = options["timeout"].asInt();
        if (ms > 0)
            reply->setTimeOut(ms);
        else
            reply->setTimeOut(5000);   // 默认超时, 否则连接挂死时 promise 永不 settle
    }
    else
    {
        reply->setTimeOut(5000);
    }

    connect(&reply->sigFinished, this, &JZJSNetModule::onNetFinish);
    m_replyMap[reply] = promiseHandle;
    m_replyUrl[reply] = url;
    m_engine->addPendingWork();
}

// sigFinished 经 JZNetObject::postEvent 回投到对象线程(=引擎线程)触发,
// 故这里直接在本线程构造 JS 结果并结算 promise, quickjs 上下文可直接访问。
void JZJSNetModule::onNetFinish()
{
    JZNetworkReply* reply = (JZNetworkReply*)(sender());

    // 抽取纯数据(不涉及 quickjs)
    int promiseHandle = -1;
    auto it = m_replyMap.find(reply);
    if (it != m_replyMap.end())
    {
        promiseHandle = it->second;
        m_replyMap.erase(it);
    }
    std::string url = m_replyUrl[reply];
    m_replyUrl.erase(reply);

    int err = reply->error();
    std::string errStr = reply->errorString();
    int status = reply->httpStatusCode();
    std::string statusText = reply->httpStatusCodeText();
    std::map<std::string, std::string> headers;
    for (auto& h : reply->headers())
        headers[h.first] = h.second;
    JZByteArray body = reply->readAll();

    reply->deleteLater();
    if (promiseHandle >= 0)
        m_engine->releasePendingWork();

    if (promiseHandle < 0)
        return;

    // 直接在本线程(引擎线程)构造 JS 结果并结算 promise
    JSContext* ctx = m_engine->context();
    if (!ctx)
        return;

    if (err != JZNetworkReply::NoError && err != JZNetworkReply::HttpError)
    {
        m_engine->setPromiseReject(promiseHandle, errStr);
        return;
    }

    JSValue jsHeaders = JS_NewObject(ctx);
    for (auto& h : headers)
        JS_SetPropertyStr(ctx, jsHeaders, h.first.c_str(), JS_NewString(ctx, h.second.c_str()));

    JSValue result = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, result, "status", JS_NewInt32(ctx, status));
    JS_SetPropertyStr(ctx, result, "statusText", JS_NewString(ctx, statusText.c_str()));
    JS_SetPropertyStr(ctx, result, "headers", jsHeaders);
    JS_SetPropertyStr(ctx, result, "body", jsMakeUint8Array(ctx, body));
    JS_SetPropertyStr(ctx, result, "url", JS_NewString(ctx, url.c_str()));

    JZJSValuePtr ptr = m_engine->makeValuePtr(result);
    std::vector<JZJSValuePtr> args = {ptr};
    m_engine->setPromiseResolve(promiseHandle, args);
}

Json::Value JZJSNetModule::urlParse(const std::string& url)
{
    JZUrl u(url);
    Json::Value res;

    std::string port_str = std::to_string(u.port());
    std::string query_str = u.query().toString();   // 已含前导 '?'
    std::string search = query_str;

    res["href"] = url;
    res["protocol"] = u.scheme().empty() ? std::string() : (u.scheme() + ":");
    res["hostname"] = u.host();
    res["port"] = port_str;
    res["host"] = port_str.empty() ? u.host() : (u.host() + ":" + port_str);
    res["pathname"] = u.path();
    res["search"] = search;
    res["path"] = u.path() + search;

    Json::Value query(Json::objectValue);
    if (!query_str.empty())
    {
        size_t pos = 0;
        while (pos <= query_str.size())
        {
            size_t amp = query_str.find('&', pos);
            std::string item = query_str.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
            if (!item.empty())
            {
                size_t eq = item.find('=');
                std::string key = (eq == std::string::npos) ? item : item.substr(0, eq);
                std::string value = (eq == std::string::npos) ? std::string() : item.substr(eq + 1);
                query[key] = value;
            }
            if (amp == std::string::npos)
                break;
            pos = amp + 1;
        }
    }
    res["query"] = query;
    return res;
}

Json::Value JZJSNetModule::osNetworkInterfaces()
{
    Json::Value res(Json::objectValue);
    auto list = JZNetworkInterface::allInterfaces();
    for (size_t i = 0; i < list.size(); i++)
    {
        Json::Value item;
        item["address"] = list[i].ip;
        item["family"] = "IPv4";
        item["internal"] = (list[i].ip.compare(0, 4, "127.") == 0);
        res[list[i].name].append(item);
    }
    return res;
}

JSValue JZJSNetModule::makeHttpResponseValue(JZJSHttpResponse* obj)
{
    return m_engine->createValue(m_typeHttpResponse, obj);
}

JSValue JZJSNetModule::httpCreateServer()
{
    JZJSHttpServer* obj = new JZJSHttpServer(m_engine, this);
    obj->server = new JZHttpServer();
    obj->handler = new JZJSHttpHandler();
    obj->handler->server = obj;
    // catch-all: 全部请求都交给 JS 的 'request' 事件，不在 C++ 侧做路由
    obj->server->addRoute("/<path:path>", obj->handler);
    addSink(&obj->sink, [obj]() { obj->close(); });

    return m_engine->createValue(m_typeHttpServer, obj);
}

JSValue JZJSNetModule::bytesFromString(const std::string& text)
{
    JSContext* ctx = m_engine->context();
    JZByteArray data = str_toUtf8(text);
    return JS_NewUint8ArrayCopy(ctx, (const uint8_t*)data.data(), data.size());
}

std::string JZJSNetModule::bytesToString(JSValue data)
{
    JZByteArray buf;
    jsValueToBytes(m_engine->context(), data, buf);
    return str_fromUtf8(buf);
}

