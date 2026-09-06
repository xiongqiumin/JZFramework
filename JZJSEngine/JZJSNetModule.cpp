#include "JZJSNetModule.h"

#include <cstdio>

#include "JZByteArray.h"
#include "JZJSBind.h"

#define NETSTEP(x) do { fprintf(stderr, "[net] %s\n", x); fflush(stderr); } while (0)

static bool jsSameValue(JSValue a, JSValue b)
{
    return a.u.ptr == b.u.ptr && JS_VALUE_GET_TAG(a) == JS_VALUE_GET_TAG(b);
}

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
    fprintf(stderr, "[sink] clear ptr=%p disposed=%d events=%d ctx=%p\n", (void*)this,
            (int)m_disposed, (int)m_listeners.size(), (void*)ctx);
    fflush(stderr);
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
        if (jsSameValue(list[i].second, cb))
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
    uint8_t* buf = JS_GetUint8Array(ctx, &size, v);
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

    fprintf(stderr, "[ws] send isConnected=%d size=%d binary=%d\n",
            socket ? (int)socket->isConnected() : -1, (int)buf.size(), (int)binary);
    fflush(stderr);

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
// JS 适配层: 把 __jz.* 原子 API 包装成 Node 风格
// ======================================================================
// MSVC 单个字符串字面量上限 16380 字节, 适配层拆成两段拼接
static const char* js_net_adapter_a = R"JSRAW(
(function () {
'use strict';

class EventEmitter {
    constructor() { this._ev = Object.create(null); }
    on(e, l) { if (!this._ev[e]) this._ev[e] = []; this._ev[e].push(l); return this; }
    once(e, l) {
        const self = this;
        function w() { self.off(e, w); l.apply(self, arguments); }
        w.__orig = l;
        return this.on(e, w);
    }
    off(e, l) {
        const a = this._ev[e];
        if (!a) return this;
        if (!l) { delete this._ev[e]; return this; }
        for (let i = a.length - 1; i >= 0; i--) {
            if (a[i] === l || a[i].__orig === l) a.splice(i, 1);
        }
        if (!a.length) delete this._ev[e];
        return this;
    }
    removeListener(e, l) { return this.off(e, l); }
    removeAllListeners(e) {
        if (e) delete this._ev[e]; else this._ev = Object.create(null);
        return this;
    }
    listenerCount(e) { return this._ev[e] ? this._ev[e].length : 0; }
    eventNames() { return Object.keys(this._ev); }
    emit(e) {
        const list = this._ev[e];
        if (!list || !list.length) {
            if (e === 'error') console.error('Unhandled error:', arguments[1]);
            return false;
        }
        const args = [];
        for (let i = 1; i < arguments.length; i++) args.push(arguments[i]);
        const arr = list.slice();
        for (let i = 0; i < arr.length; i++) {
            try { arr[i].apply(this, args); }
            catch (err) { console.error('listener error:', err); }
        }
        return true;
    }
}
globalThis.EventEmitter = EventEmitter;

function toBytes(data, enc) {
    if (data === null || data === undefined) return new Uint8Array(0);
    if (typeof data === 'string') {
        if (enc === 'hex') return Uint8Array.fromHex(data);
        if (enc === 'base64') return Uint8Array.fromBase64(data);
        if (enc === 'latin1') {
            const r = new Uint8Array(data.length);
            for (let i = 0; i < data.length; i++) r[i] = data.charCodeAt(i) & 0xff;
            return r;
        }
        return __jz.bytes.fromString(data);
    }
    if (data instanceof Uint8Array) return data;
    if (data instanceof ArrayBuffer) return new Uint8Array(data);
    throw new TypeError('data must be string, Uint8Array or ArrayBuffer');
}
function fromBytes(u8, enc) {
    if (!enc || enc === 'utf8' || enc === 'utf-8') return __jz.bytes.toString(u8);
    if (enc === 'hex') return u8.toHex();
    if (enc === 'base64') return u8.toBase64();
    if (enc === 'latin1') {
        let s = '';
        for (let i = 0; i < u8.length; i++) s += String.fromCharCode(u8[i]);
        return s;
    }
    throw new TypeError('unknown encoding: ' + enc);
}
function isEnc(v) { return typeof v === 'string'; }

// ---------------------------------------------------------------- net
class Socket extends EventEmitter {
    constructor(h) {
        super();
        this._h = h;
        this._enc = null;
        this._timeout = 0;
        this._timer = 0;
        h.on('connect', () => this.emit('connect'));
        h.on('data', (b) => {
            if (this._timeout > 0) this._armTimeout();
            this.emit('data', this._enc ? fromBytes(b, this._enc) : b);
        });
        h.on('end', () => this.emit('end'));
        h.on('close', () => { this._disarm(); this.emit('close'); });
        h.on('error', (m) => this.emit('error', new Error(m)));
    }
    _armTimeout() {
        this._disarm();
        if (this._timeout > 0) this._timer = setTimeout(() => this.emit('timeout'), this._timeout);
    }
    _disarm() { if (this._timer) { clearTimeout(this._timer); this._timer = 0; } }
    connect(a, b, c) {
        let opts = {};
        let cb = null;
        if (typeof a === 'number') { opts = { port: a, host: b || '127.0.0.1' }; cb = c; }
        else { opts = a || {}; cb = b; }
        if (cb) this.on('connect', cb);
        this._armTimeout();
        this._h.connectTo(opts.port | 0, opts.host || '127.0.0.1');
        return this;
    }
    write(data, enc, cb) {
        const ok = this._h.write(toBytes(data, isEnc(enc) ? enc : null));
        if (typeof enc === 'function') enc();
        else if (cb) cb();
        return ok;
    }
    end(data, enc, cb) {
        if (data !== undefined && data !== null) this._h.write(toBytes(data, isEnc(enc) ? enc : null));
        this._h.end();
        if (typeof data === 'function') data();
        else if (typeof enc === 'function') enc();
        else if (cb) cb();
        return this;
    }
    destroy() { this._disarm(); this._h.destroy(); return this; }
    setTimeout(ms, cb) { this._timeout = ms | 0; if (cb) this.on('timeout', cb); this._armTimeout(); return this; }
    setEncoding(enc) { this._enc = enc || null; return this; }
    get remoteAddress() { return this._h.remoteAddress; }
    get remotePort() { return this._h.remotePort; }
    get localAddress() { return this._h.localAddress; }
    get localPort() { return this._h.localPort; }
    get bytesRead() { return this._h.bytesRead; }
    get bytesWritten() { return this._h.bytesWritten; }
    get destroyed() { return this._h.destroyed; }
    get readyState() {
        const s = this._h.state;
        return s === 1 ? 'open' : (s === 3 ? 'closed' : 'opening');
    }
}

class Server extends EventEmitter {
    constructor(h) {
        super();
        this._h = h;
        this.maxConnections = 0;
        h.on('connection', (s) => this.emit('connection', new Socket(s)));
        h.on('listening', () => this.emit('listening'));
        h.on('close', () => setTimeout(() => this.emit('close'), 0));
    }
    listen(a, b, c) {
        let port = 0, host = '', cb = null;
        if (a && typeof a === 'object') { port = a.port || 0; host = a.host || ''; cb = b; }
        else { port = a || 0; if (typeof b === 'string') { host = b; cb = c; } else cb = b; }
        if (cb) this.on('listening', cb);
        this._h.listen(port | 0, host);
        return this;
    }
    close(cb) { if (cb) this.on('close', cb); this._h.close(); return this; }
    address() { const a = this._h.address; return (a && a.port) ? a : null; }
    get listening() { return this._h.listening; }
}

const net = {
    Socket: Socket,
    Server: Server,
    createServer(a, b) {
        const cb = (typeof a === 'function') ? a : b;
        const opts = (a && typeof a === 'object') ? a : {};
        const s = new Server(__jz.net.createServer());
        if (cb) s.on('connection', cb);
        if (opts.port) s.listen(opts.port, opts.host);
        return s;
    },
    createConnection(a, b, c) {
        let opts, cb = null;
        if (typeof a === 'number') { opts = { port: a, host: b || '127.0.0.1' }; cb = c; }
        else { opts = a || {}; cb = b; }
        const s = new Socket(__jz.net.createSocket());
        if (cb) s.on('connect', cb);
        s.connect(opts);
        return s;
    }
};
net.connect = net.createConnection;

// -------------------------------------------------------------- dgram
class UdpSocket extends EventEmitter {
    constructor(h) {
        super();
        this._h = h;
        this._enc = null;
        h.on('message', (b, rinfo) => {
            this.emit('message', this._enc ? fromBytes(b, this._enc) : b, rinfo);
        });
        h.on('listening', () => this.emit('listening'));
        h.on('close', () => setTimeout(() => this.emit('close'), 0));
        h.on('error', (m) => this.emit('error', new Error(m)));
    }
    bind(a, b, c) {
        let port = 0, cb = null;
        if (a && typeof a === 'object') { port = a.port || 0; cb = b; }
        else { port = a || 0; cb = (typeof b === 'function') ? b : c; }
        if (cb) this.on('listening', cb);
        this._h.bind(port | 0);
        return this;
    }
    send(msg, a, b, c, d, e) {
        let port, addr, cb = null, data = toBytes(msg, null);
        if (typeof a === 'number' && typeof b === 'number') {
            const off = a, len = b;
            data = data.subarray(off, off + len);
            port = c; addr = d; cb = e;
        } else {
            port = a; addr = b; cb = c;
        }
        this._h.send(addr || '127.0.0.1', port | 0, data);
        if (cb) cb();
    }
    close(cb) { if (cb) this.on('close', cb); this._h.close(); return this; }
    address() { return this._h.address; }
    setEncoding(enc) { this._enc = enc || null; return this; }
}

const dgram = {
    Socket: UdpSocket,
    createSocket(type, listener) {
        if (type !== 'udp4') throw new TypeError('only udp4 is supported');
        const s = new UdpSocket(__jz.dgram.createSocket());
        if (listener) s.on('message', listener);
        return s;
    }
};

// --------------------------------------------------------------- http
class Headers {
    constructor(init) {
        this._m = Object.create(null);
        if (init) {
            if (typeof init.forEach === 'function' && !Array.isArray(init)) {
                init.forEach((v, k) => { this._m[String(k).toLowerCase()] = String(v); });
            } else {
                for (const k in init) this._m[String(k).toLowerCase()] = String(init[k]);
            }
        }
    }
    get(n) { const v = this._m[String(n).toLowerCase()]; return v === undefined ? null : v; }
    has(n) { return this._m[String(n).toLowerCase()] !== undefined; }
    set(n, v) { this._m[String(n).toLowerCase()] = String(v); }
    append(n, v) {
        const k = String(n).toLowerCase();
        this._m[k] = (this._m[k] === undefined) ? String(v) : this._m[k] + ', ' + String(v);
    }
    delete(n) { delete this._m[String(n).toLowerCase()]; }
    keys() { return Object.keys(this._m); }
    values() { const r = []; for (const k in this._m) r.push(this._m[k]); return r; }
    entries() { const r = []; for (const k in this._m) r.push([k, this._m[k]]); return r; }
    forEach(cb) { for (const k in this._m) cb(this._m[k], k, this); }
    toJSON() { const r = {}; for (const k in this._m) r[k] = this._m[k]; return r; }
}
globalThis.Headers = Headers;

class IncomingMessage extends EventEmitter {
    constructor(info) {
        super();
        this.statusCode = info.status;
        this.statusMessage = info.statusText;
        this.headers = new Headers(info.headers);
        this.httpVersion = '1.1';
        this.url = info.url;
        this._body = info.body;
        this._enc = null;
    }
    setEncoding(enc) { this._enc = enc || null; return this; }
    _start() {
        this.emit('data', this._enc ? fromBytes(this._body, this._enc) : this._body);
        this.emit('end');
    }
}

class ClientRequest extends EventEmitter {
    constructor(opts) {
        super();
        this._opts = opts || {};
        this._headers = {};
        if (this._opts.headers) for (const k in this._opts.headers) this._headers[k] = this._opts.headers[k];
        this._body = new Uint8Array(0);
        this._sent = false;
    }
    setHeader(k, v) { this._headers[k] = String(v); }
    getHeader(k) { return this._headers[k]; }
    removeHeader(k) { delete this._headers[k]; }
    write(chunk, enc, cb) {
        const b = toBytes(chunk, isEnc(enc) ? enc : null);
        const n = new Uint8Array(this._body.length + b.length);
        n.set(this._body, 0); n.set(b, this._body.length);
        this._body = n;
        if (typeof enc === 'function') enc(); else if (cb) cb();
        return true;
    }
    end(data, enc, cb) {
        if (data !== undefined && data !== null && typeof data !== 'function') {
            const e = (typeof enc === 'string') ? enc : null;
            this.write(data, e);
        }
        if (typeof data === 'function') data();
        else if (typeof enc === 'function') enc();
        else if (cb) cb();
        this._send();
        return this;
    }
    destroy() { this._sent = true; return this; }
    abort() { this._sent = true; }
    setTimeout(ms, cb) { if (this._opts) this._opts.timeout = ms; if (cb) this.on('timeout', cb); return this; }
    _send() {
        if (this._sent) return;
        this._sent = true;
        const self = this;
        const req = {
            url: this._opts.url || '',
            method: this._opts.method || 'GET',
            headers: this._headers,
            timeout: this._opts.timeout || 0
        };
        __jz.http.request(req, this._body).then(function (info) {
            const res = new IncomingMessage(info);
            self.emit('response', res);
            res._start();
        }, function (err) {
            self.emit('error', err instanceof Error ? err : new Error(String(err)));
        });
    }
}

function httpRequest(url, options, cb) {
    let opts = {}, callback = null;
    if (typeof url === 'string') {
        if (options && typeof options === 'object') { opts = options; opts.url = url; }
        else { opts = { url: url }; callback = options; }
    } else {
        opts = url || {};
        callback = options;
    }
    const req = new ClientRequest(opts);
    if (callback) req.on('response', callback);
    if (opts.method === undefined) req._opts.method = 'GET';
    if (!cb && (opts.method === 'GET' || opts.method === undefined)) {
        // http.get 语义: 立即发送
        req._send();
    }
    return req;
}

const http = {
    IncomingMessage: IncomingMessage,
    ClientRequest: ClientRequest,
    METHODS: ['GET', 'POST'],
    STATUS_CODES: {
        100: 'Continue', 101: 'Switching Protocols', 200: 'OK', 201: 'Created',
        202: 'Accepted', 204: 'No Content', 206: 'Partial Content',
        301: 'Moved Permanently', 302: 'Found', 304: 'Not Modified',
        400: 'Bad Request', 401: 'Unauthorized', 403: 'Forbidden', 404: 'Not Found',
        405: 'Method Not Allowed', 408: 'Request Timeout', 409: 'Conflict',
        413: 'Payload Too Large', 415: 'Unsupported Media Type',
        500: 'Internal Server Error', 501: 'Not Implemented',
        502: 'Bad Gateway', 503: 'Service Unavailable', 504: 'Gateway Timeout'
    },
    request: function (url, options, cb) { return httpRequest(url, options, cb); },
    get: function (url, options, cb) {
        const req = httpRequest(url, options, cb);
        req._opts.method = 'GET';
        req._send();
        return req;
    }
};

// -------------------------------------------------------------- fetch
class Response {
    constructor(info) {
        this.status = info.status;
        this.statusText = info.statusText;
        this.ok = this.status >= 200 && this.status < 300;
        this.url = info.url;
        this.headers = new Headers(info.headers);
        this._body = info.body;
        this.bodyUsed = false;
    }
    _take() { this.bodyUsed = true; return this._body; }
    arrayBuffer() { const b = this._take(); return Promise.resolve(b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength)); }
    uint8Array() { return Promise.resolve(this._take()); }
    text() { return Promise.resolve(__jz.bytes.toString(this._take())); }
    json() { const s = __jz.bytes.toString(this._take()); return Promise.resolve(JSON.parse(s)); }
    static error() { return new Response({ status: 0, statusText: '', headers: {}, body: new Uint8Array(0), url: '' }); }
}
globalThis.Response = Response;

globalThis.fetch = function (url, init) {
    init = init || {};
    let body = new Uint8Array(0);
    if (init.body !== undefined && init.body !== null) body = toBytes(init.body, null);
    const req = {
        url: String(url),
        method: init.method || (body.length ? 'POST' : 'GET'),
        headers: init.headers || {},
        timeout: init.timeout || 0
    };
    return __jz.http.request(req, body).then(function (info) { return new Response(info); });
};

)JSRAW";

static const char* js_net_adapter_b = R"JSRAW(
// ---------------------------------------------------------- websocket
class WebSocketExt extends EventEmitter {
    constructor(arg, protocols, opts) {
        super();
        let h = arg;
        if (typeof arg === 'string') {
            // 公开用法: new WebSocket(url) —— 内部由 C++ 创建连接并返回句柄
            h = __jz.ws.connect(arg);
        }
        this._h = h;
        this.onopen = null;
        this.onmessage = null;
        this.onclose = null;
        this.onerror = null;
        h.on('open', () => { this.emit('open'); if (this.onopen) this.onopen({ target: this }); });
        h.on('message', (d) => { this.emit('message', d); if (this.onmessage) this.onmessage({ data: d, target: this }); });
        h.on('close', (code, reason) => {
            this.emit('close', code, reason);
            if (this.onclose) this.onclose({ code: code, reason: reason, wasClean: code === 1000 });
        });
        h.on('error', (m) => {
            const err = new Error(m);
            this.emit('error', err);
            if (this.onerror) this.onerror({ error: err, message: m });
        });
    }
    get readyState() { return this._h.state; }
    get url() { return this._h.url; }
    get protocol() { return ''; }
    send(data) {
        if (typeof data === 'string')
            this._h.send(data, false);
        else
            this._h.send(toBytes(data, null), true);
    }
    close(code, reason) { this._h.close(); }
    ping(data) { this._h.ping(); }
}
WebSocketExt.CONNECTING = 0;
WebSocketExt.OPEN = 1;
WebSocketExt.CLOSING = 2;
WebSocketExt.CLOSED = 3;
globalThis.WebSocket = WebSocketExt;

class WebSocketServer extends EventEmitter {
    constructor(opts) {
        super();
        this._h = __jz.ws.createServer();
        const h = this._h;
        h.on('connection', (s) => this.emit('connection', new WebSocketExt(s)));
        h.on('listening', () => this.emit('listening'));
        h.on('close', () => setTimeout(() => this.emit('close'), 0));
        if (opts && opts.port) h.listen(opts.port | 0, opts.host || '');
    }
    close(cb) { if (cb) this.on('close', cb); this._h.close(); }
    address() { const a = this._h.address; return (a && a.port) ? a : null; }
}

const ws = {
    WebSocket: WebSocketExt,
    WebSocketServer: WebSocketServer,
    Server: WebSocketServer,
    createServer(opts, cb) {
        const s = new WebSocketServer(opts);
        if (cb) s.on('listening', cb);
        return s;
    }
};

// --------------------------------------------------------------- mqtt
class MqttClient extends EventEmitter {
    constructor(h) {
        super();
        this._h = h;
        h.on('connect', () => this.emit('connect'));
        h.on('message', (topic, payload, qos) => {
            this.emit('message', topic, payload, { topic: topic, payload: payload, qos: qos, retain: false });
        });
        h.on('close', () => { this.emit('close'); this.emit('end'); });
    }
    publish(topic, message, a, b) {
        let qos = 0, retain = false, cb = null;
        if (typeof a === 'object' && a !== null) { qos = a.qos || 0; retain = !!a.retain; cb = b; }
        else if (typeof a === 'function') cb = a;
        this._h.publish(topic, toBytes(message, null), qos | 0, retain);
        if (cb) cb();
        return this;
    }
    subscribe(topic, a, b) {
        let qos = 0, cb = null;
        if (typeof a === 'object' && a !== null) { qos = a.qos || 0; cb = b; }
        else if (typeof a === 'function') cb = a;
        const list = Array.isArray(topic) ? topic : [topic];
        for (let i = 0; i < list.length; i++) this._h.subscribe(list[i], qos | 0);
        if (cb) cb(null, list.map(function (t) { return { topic: t, qos: qos | 0 }; }));
        return this;
    }
    unsubscribe(topic, cb) {
        const list = Array.isArray(topic) ? topic : [topic];
        for (let i = 0; i < list.length; i++) this._h.unsubscribe(list[i]);
        if (cb) cb();
        return this;
    }
    end(force, cb) {
        if (typeof force === 'function') force();
        else if (cb) cb();
        this._h.end();
        return this;
    }
    get connected() { return this._h.connected; }
}

const mqtt = {
    MqttClient: MqttClient,
    connect(url, opts) {
        const o = opts || {};
        const h = __jz.mqtt.connect(String(url), {
            clientId: o.clientId || '',
            username: o.username || '',
            password: o.password || '',
            keepalive: o.keepalive || 60,
            clean: o.clean === undefined ? true : !!o.clean,
            protocolVersion: o.protocolVersion || 4,
            will: o.will ? {
                topic: o.will.topic || '',
                payload: toBytes(o.will.payload === undefined ? '' : o.will.payload, null).toBase64(),
                qos: o.will.qos || 0,
                retain: !!o.will.retain
            } : null
        });
        return new MqttClient(h);
    }
};

// ---------------------------------------------------------------- url
class URLSearchParams {
    constructor(init) {
        this._m = Object.create(null);
        if (!init) return;
        if (typeof init === 'string') {
            const s = init.charAt(0) === '?' ? init.slice(1) : init;
            if (!s) return;
            const parts = s.split('&');
            for (let i = 0; i < parts.length; i++) {
                if (!parts[i]) continue;
                const p = parts[i].indexOf('=');
                if (p < 0) this._m[decodeURIComponent(parts[i])] = '';
                else this._m[decodeURIComponent(parts[i].slice(0, p))] = decodeURIComponent(parts[i].slice(p + 1));
            }
        } else {
            for (const k in init) this._m[k] = String(init[k]);
        }
    }
    get(n) { const v = this._m[n]; return v === undefined ? null : v; }
    has(n) { return this._m[n] !== undefined; }
    set(n, v) { this._m[n] = String(v); }
    append(n, v) {
        this._m[n] = (this._m[n] === undefined) ? String(v) : this._m[n] + ',' + String(v);
    }
    delete(n) { delete this._m[n]; }
    keys() { return Object.keys(this._m); }
    values() { const r = []; for (const k in this._m) r.push(this._m[k]); return r; }
    entries() { const r = []; for (const k in this._m) r.push([k, this._m[k]]); return r; }
    forEach(cb) { for (const k in this._m) cb(this._m[k], k, this); }
    toString() {
        const r = [];
        for (const k in this._m) r.push(encodeURIComponent(k) + '=' + encodeURIComponent(this._m[k]));
        return r.join('&');
    }
}
globalThis.URLSearchParams = URLSearchParams;

class URL {
    constructor(u) {
        const p = __jz.url.parse(String(u));
        this._p = p;
        this.href = p.href;
        this.searchParams = new URLSearchParams(p.search);
    }
    get protocol() { return this._p.protocol; }
    get hostname() { return this._p.hostname; }
    get host() { return this._p.host; }
    get port() { return this._p.port; }
    get pathname() { return this._p.pathname; }
    get search() { return this._p.search; }
    get path() { return this._p.path; }
    toString() { return this.href; }
}
globalThis.URL = URL;

const url = {
    URL: URL,
    URLSearchParams: URLSearchParams,
    parse: function (u) { return __jz.url.parse(String(u)); }
};

// ----------------------------------------------------------------- os
const os = {
    networkInterfaces: function () { return __jz.os.networkInterfaces(); }
};

// ------------------------------------------------------------ require
const modules = { net: net, dgram: dgram, http: http, mqtt: mqtt, url: url, os: os, ws: ws };
globalThis.require = function (name) {
    const m = modules[name];
    if (!m) throw new Error("Cannot find module '" + name + "'");
    return m;
};
globalThis.net = net;
globalThis.dgram = dgram;
globalThis.http = http;
globalThis.mqtt = mqtt;
globalThis.url = url;
globalThis.os = os;
globalThis.ws = ws;

})();
)JSRAW";

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
    m_cbRoot = JS_UNDEFINED;
    m_cbId = 0;
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
    JSContext* ctx = m_engine->context();
    int id = m_cbId++;
    // 根数组持有引用, 使闭包对 GC 始终可达。
    // 注意: JS_SetProperty* 会消费传入值的引用, cb 是借来的参数, 必须先 dup 一份。
    JS_SetPropertyInt64(ctx, m_cbRoot, id, JS_DupValue(ctx, cb));
    return id;
}

void JZJSNetModule::releaseCallback(int id, JSValue value)
{
    JSContext* ctx = m_engine->context();
    fprintf(stderr, "[cb] release id=%d rootTag=%d valTag=%d\n", id,
            (int)JS_VALUE_GET_TAG(m_cbRoot), (int)JS_VALUE_GET_TAG(value));
    fflush(stderr);
    if (id >= 0)
        JS_SetPropertyInt64(ctx, m_cbRoot, id, JS_UNDEFINED);   // 释放根数组对该回调的引用
    JS_FreeValue(ctx, value);
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

    // JS 回调的 GC 根数组(挂在全局, 防止"只被 C++ 持有"的闭包被 GC 回收)。
    // setProperty 会消费传入值的引用, 这里传 dup 出去的一份, 自己仍持有一份。
    m_cbRoot = JS_NewArray(m_engine->context());
    m_engine->setProperty(global, "__jzCallbackRoot", JS_DupValue(m_engine->context(), m_cbRoot));

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
    NETSTEP("regist socket class");
    m_engine->registClass(bind_socket.classDefine());
    m_typeSocket = m_engine->ctypeidToType(typeid(JZJSNetSocket).name());
    NETSTEP("socket class done");

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
    NETSTEP("regist mqtt class");
    m_engine->registClass(bind_mqtt.classDefine());
    m_typeMqtt = m_engine->ctypeidToType(typeid(JZJSMqttClient).name());

    // ---- __jz 命名空间 ----
    NETSTEP("namespaces");
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
    NETSTEP("js adapter eval");

    // ---- JS 适配层 ----
    std::string adapter = std::string("(function(){ ") + js_net_adapter_a + js_net_adapter_b + " })();";
    m_engine->eval(adapter);
}

void JZJSNetModule::deinitEngine()
{
    NETSTEP("deinit: clear sinks");
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
        fprintf(stderr, "[net] dispose %d/%d\n", i, (int)disposers.size());
        fflush(stderr);
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

    JSContext* ctx = m_engine ? m_engine->context() : nullptr;
    if (ctx)
    {
        for (size_t i = 0; i < pending.size(); i++)
        {
            if (pending[i].first >= 0)
                JS_SetPropertyInt64(ctx, m_cbRoot, pending[i].first, JS_UNDEFINED);
            JS_FreeValue(ctx, pending[i].second);
        }
    }

    // 主动跑一轮 GC: 让已经不可达的 JS 包装对象在 ctx 还有效时析构,
    // 否则它们的 finalizer 会在 JS_FreeContext 之后才跑, 用到已释放的 ctx。
    if (ctx)
        JS_RunGC(JS_GetRuntime(ctx));

    NETSTEP("deinit: clear replies");
    auto reply_map = m_replyMap;
    for (auto& pair : reply_map)
    {
        pair.first->close();
        pair.first->deleteLater();
    }
    m_replyMap.clear();
    m_replyUrl.clear();

    if (m_engine)
    {
        JSContext* ctx = m_engine->context();
        if (ctx)
        {
            JS_SetPropertyStr(ctx, m_engine->globalObject(), "__jzCallbackRoot", JS_UNDEFINED);
            JS_FreeValue(ctx, m_cbRoot);
        }
        m_cbRoot = JS_UNDEFINED;
    }
    m_engine = nullptr;
    NETSTEP("deinit: done");
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
            fprintf(stderr, "[ws] accept conn=%p\n", (void*)conn);
            fflush(stderr);
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
