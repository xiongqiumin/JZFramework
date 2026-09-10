/**
 * @jz/net —— 内部实现 (internal definition)
 *
 * 把引擎注入的 __jz.net / __jz.dgram / __jz.http / __jz.ws / __jz.mqtt /
 * __jz.url / __jz.os 原子 API 包装成 Node 风格模块:
 *   net / dgram / http / mqtt / ws / url / os
 * 同时注册全局变量(net/dgram/...)与 require(), 并把 fetch/WebSocket/URL/
 * URLSearchParams/Headers/Response 等 WHATWG 全局也挂到 globalThis。
 *
 * 本文件由 tsc 编译为 dist/index.js, 在 JZJSNetModule::initEngine 时
 * 通过 JZJSEngine::runModuleFile 以分离式(不内嵌进 C++)加载。
 */
'use strict';
// ---------------------------------------------------------------------------
// 共享事件发射器(与 Node events.EventEmitter 同名 API)
// ---------------------------------------------------------------------------
class EventEmitter {
    constructor() { this._ev = Object.create(null); }
    on(e, l) {
        if (!this._ev[e])
            this._ev[e] = [];
        this._ev[e].push(l);
        return this;
    }
    once(e, l) {
        const self = this;
        function w() { self.off(e, w); l.apply(self, arguments); }
        w.__orig = l;
        return this.on(e, w);
    }
    off(e, l) {
        const a = this._ev[e];
        if (!a)
            return this;
        if (!l) {
            delete this._ev[e];
            return this;
        }
        for (let i = a.length - 1; i >= 0; i--) {
            if (a[i] === l || a[i].__orig === l)
                a.splice(i, 1);
        }
        if (!a.length)
            delete this._ev[e];
        return this;
    }
    removeListener(e, l) { return this.off(e, l); }
    removeAllListeners(e) {
        if (e)
            delete this._ev[e];
        else
            this._ev = Object.create(null);
        return this;
    }
    listenerCount(e) { return this._ev[e] ? this._ev[e].length : 0; }
    eventNames() { return Object.keys(this._ev); }
    emit(e, ...args) {
        const list = this._ev[e];
        if (!list || !list.length) {
            if (e === 'error')
                console.error('Unhandled error:', args[0]);
            return false;
        }
        const arr = list.slice();
        for (let i = 0; i < arr.length; i++) {
            try {
                arr[i].apply(this, args);
            }
            catch (err) {
                console.error('listener error:', err);
            }
        }
        return true;
    }
}
globalThis.EventEmitter = EventEmitter;
function toBytes(data, enc) {
    if (data === null || data === undefined)
        return new Uint8Array(0);
    if (typeof data === 'string') {
        if (enc === 'hex')
            return Uint8Array.fromHex(data);
        if (enc === 'base64')
            return Uint8Array.fromBase64(data);
        if (enc === 'latin1') {
            const r = new Uint8Array(data.length);
            for (let i = 0; i < data.length; i++)
                r[i] = data.charCodeAt(i) & 0xff;
            return r;
        }
        return __jz.bytes.fromString(data);
    }
    if (data instanceof Uint8Array)
        return data;
    if (data instanceof ArrayBuffer)
        return new Uint8Array(data);
    throw new TypeError('data must be string, Uint8Array or ArrayBuffer');
}
function fromBytes(u8, enc) {
    if (!enc || enc === 'utf8' || enc === 'utf-8')
        return __jz.bytes.toString(u8);
    if (enc === 'hex')
        return u8.toHex();
    if (enc === 'base64')
        return u8.toBase64();
    if (enc === 'latin1') {
        let s = '';
        for (let i = 0; i < u8.length; i++)
            s += String.fromCharCode(u8[i]);
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
            if (this._timeout > 0)
                this._armTimeout();
            this.emit('data', this._enc ? fromBytes(b, this._enc) : b);
        });
        h.on('end', () => this.emit('end'));
        h.on('close', () => { this._disarm(); this.emit('close'); });
        h.on('error', (m) => this.emit('error', new Error(m)));
    }
    _armTimeout() {
        this._disarm();
        if (this._timeout > 0)
            this._timer = setTimeout(() => this.emit('timeout'), this._timeout);
    }
    _disarm() { if (this._timer) {
        clearTimeout(this._timer);
        this._timer = 0;
    } }
    connect(a, b, c) {
        let opts = {};
        let cb = null;
        if (typeof a === 'number') {
            opts = { port: a, host: b || '127.0.0.1' };
            cb = c;
        }
        else {
            opts = a || {};
            cb = b;
        }
        if (cb)
            this.on('connect', cb);
        this._armTimeout();
        this._h.connectTo(opts.port | 0, opts.host || '127.0.0.1');
        return this;
    }
    write(data, enc, cb) {
        const ok = this._h.write(toBytes(data, isEnc(enc) ? enc : null));
        if (typeof enc === 'function')
            enc();
        else if (cb)
            cb();
        return ok;
    }
    end(data, enc, cb) {
        if (data !== undefined && data !== null)
            this._h.write(toBytes(data, isEnc(enc) ? enc : null));
        this._h.end();
        if (typeof data === 'function')
            data();
        else if (typeof enc === 'function')
            enc();
        else if (cb)
            cb();
        return this;
    }
    destroy() { this._disarm(); this._h.destroy(); return this; }
    setTimeout(ms, cb) { this._timeout = ms | 0; if (cb)
        this.on('timeout', cb); this._armTimeout(); return this; }
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
        if (a && typeof a === 'object') {
            port = a.port || 0;
            host = a.host || '';
            cb = b;
        }
        else {
            port = a || 0;
            if (typeof b === 'string') {
                host = b;
                cb = c;
            }
            else
                cb = b;
        }
        if (cb)
            this.on('listening', cb);
        this._h.listen(port | 0, host);
        return this;
    }
    close(cb) { if (cb)
        this.on('close', cb); this._h.close(); return this; }
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
        if (cb)
            s.on('connection', cb);
        if (opts.port)
            s.listen(opts.port, opts.host);
        return s;
    },
    createConnection(a, b, c) {
        let opts, cb = null;
        if (typeof a === 'number') {
            opts = { port: a, host: b || '127.0.0.1' };
            cb = c;
        }
        else {
            opts = a || {};
            cb = b;
        }
        const s = new Socket(__jz.net.createSocket());
        if (cb)
            s.on('connect', cb);
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
        if (a && typeof a === 'object') {
            port = a.port || 0;
            cb = b;
        }
        else {
            port = a || 0;
            cb = (typeof b === 'function') ? b : c;
        }
        if (cb)
            this.on('listening', cb);
        this._h.bind(port | 0);
        return this;
    }
    send(msg, a, b, c, d, e) {
        let port, addr, cb = null, data = toBytes(msg, null);
        if (typeof a === 'number' && typeof b === 'number') {
            const off = a, len = b;
            data = data.subarray(off, off + len);
            port = c;
            addr = d;
            cb = e;
        }
        else {
            port = a;
            addr = b;
            cb = c;
        }
        this._h.send(addr || '127.0.0.1', port | 0, data);
        if (cb)
            cb();
    }
    close(cb) { if (cb)
        this.on('close', cb); this._h.close(); return this; }
    address() { return this._h.address; }
    setEncoding(enc) { this._enc = enc || null; return this; }
}
const dgram = {
    Socket: UdpSocket,
    createSocket(type, listener) {
        if (type !== 'udp4')
            throw new TypeError('only udp4 is supported');
        const s = new UdpSocket(__jz.dgram.createSocket());
        if (listener)
            s.on('message', listener);
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
            }
            else {
                for (const k in init)
                    this._m[String(k).toLowerCase()] = String(init[k]);
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
    values() { const r = []; for (const k in this._m)
        r.push(this._m[k]); return r; }
    entries() { const r = []; for (const k in this._m)
        r.push([k, this._m[k]]); return r; }
    forEach(cb) { for (const k in this._m)
        cb(this._m[k], k, this); }
    toJSON() { const r = {}; for (const k in this._m)
        r[k] = this._m[k]; return r; }
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
        if (this._opts.headers)
            for (const k in this._opts.headers)
                this._headers[k] = this._opts.headers[k];
        this._body = new Uint8Array(0);
        this._sent = false;
    }
    setHeader(k, v) { this._headers[k] = String(v); }
    getHeader(k) { return this._headers[k]; }
    removeHeader(k) { delete this._headers[k]; }
    write(chunk, enc, cb) {
        const b = toBytes(chunk, isEnc(enc) ? enc : null);
        const n = new Uint8Array(this._body.length + b.length);
        n.set(this._body, 0);
        n.set(b, this._body.length);
        this._body = n;
        if (typeof enc === 'function')
            enc();
        else if (cb)
            cb();
        return true;
    }
    end(data, enc, cb) {
        if (data !== undefined && data !== null && typeof data !== 'function') {
            const e = (typeof enc === 'string') ? enc : null;
            this.write(data, e);
        }
        if (typeof data === 'function')
            data();
        else if (typeof enc === 'function')
            enc();
        else if (cb)
            cb();
        this._send();
        return this;
    }
    destroy() { this._sent = true; return this; }
    abort() { this._sent = true; }
    setTimeout(ms, cb) { if (this._opts)
        this._opts.timeout = ms; if (cb)
        this.on('timeout', cb); return this; }
    _send() {
        if (this._sent)
            return;
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
        if (options && typeof options === 'object') {
            opts = options;
            opts.url = url;
        }
        else {
            opts = { url: url };
            callback = options;
        }
    }
    else {
        opts = url || {};
        callback = options;
    }
    const req = new ClientRequest(opts);
    if (callback)
        req.on('response', callback);
    if (opts.method === undefined)
        req._opts.method = 'GET';
    if (!cb && (opts.method === 'GET' || opts.method === undefined)) {
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
    if (init.body !== undefined && init.body !== null)
        body = toBytes(init.body, null);
    const req = {
        url: String(url),
        method: init.method || (body.length ? 'POST' : 'GET'),
        headers: init.headers || {},
        timeout: init.timeout || 0
    };
    return __jz.http.request(req, body).then(function (info) { return new Response(info); });
};
// ------------------------------------------------- http server
class ServerRequest extends EventEmitter {
    constructor(info) {
        super();
        this.method = info.method;
        this.url = info.url;
        this.headers = new Headers(info.headers);
        this.httpVersion = '1.1';
        this.remoteAddress = info.remoteAddress || '';
        this._body = info.body;
        this._enc = null;
    }
    setEncoding(enc) { this._enc = enc || null; return this; }
    get text() { return fromBytes(this._body, this._enc); }
    get body() { return this._body; }
    _start() {
        this.emit('data', this._enc ? fromBytes(this._body, this._enc) : this._body);
        this.emit('end');
    }
}
class ServerResponse extends EventEmitter {
    constructor(h) {
        super();
        this._h = h;
        this._ended = false;
    }
    get statusCode() { return this._h.statusCode; }
    set statusCode(v) { this._h.statusCode = v | 0; }
    get finished() { return this._ended; }
    setHeader(k, v) { this._h.setHeader(String(k), String(v)); return this; }
    writeHead(status, headers) {
        this._h.writeHead(status | 0, headers || {});
        return this;
    }
    write(chunk, enc, cb) {
        if (this._ended)
            return false;
        this._h.write(toBytes(chunk, isEnc(enc) ? enc : null));
        if (typeof enc === 'function')
            enc();
        else if (cb)
            cb();
        return true;
    }
    end(data, enc, cb) {
        if (this._ended)
            return this;
        const hasData = data !== undefined && data !== null && typeof data !== 'function';
        this._h.end(hasData ? toBytes(data, isEnc(enc) ? enc : null) : new Uint8Array(0));
        this._ended = true;
        if (typeof data === 'function')
            data();
        else if (typeof enc === 'function')
            enc();
        else if (cb)
            cb();
        this.emit('finish');
        return this;
    }
}
class HttpServer extends EventEmitter {
    constructor(h) {
        super();
        this._h = h;
        h.on('request', (rq, rs) => {
            const req = new ServerRequest(rq);
            const res = new ServerResponse(rs);
            this.emit('request', req, res);
            req._start();
        });
        h.on('listening', () => this.emit('listening'));
        h.on('close', () => this.emit('close'));
        h.on('error', (m) => this.emit('error', new Error(m)));
    }
    get listening() { return this._h.listening; }
    address() { return this._h.address; }
    listen(a, b, c) {
        let port = 0, host = '', cb = null;
        if (typeof a === 'number') {
            port = a;
            if (typeof b === 'string')
                host = b;
            else
                cb = b;
            if (typeof c === 'function')
                cb = c;
        }
        else if (a && typeof a === 'object') {
            port = a.port | 0;
            host = a.host || '';
            cb = b;
        }
        if (cb)
            this.on('listening', cb);
        this._h.listen(port, host);
        return this;
    }
    close(cb) {
        if (cb)
            this.on('close', cb);
        this._h.close();
        return this;
    }
}
http.createServer = function (opts, listener) {
    const s = new HttpServer(__jz.http.createServer());
    let cb = listener;
    if (typeof opts === 'function')
        cb = opts;
    if (cb)
        s.on('request', cb);
    return s;
};
http.Server = HttpServer;
http.ServerResponse = ServerResponse;
// ---------------------------------------------------------- websocket
class WebSocketExt extends EventEmitter {
    constructor(arg, protocols, opts) {
        super();
        let h = arg;
        if (typeof arg === 'string') {
            h = __jz.ws.connect(arg);
        }
        this._h = h;
        this.onopen = null;
        this.onmessage = null;
        this.onclose = null;
        this.onerror = null;
        h.on('open', () => { this.emit('open'); if (this.onopen)
            this.onopen({ target: this }); });
        h.on('message', (d) => { this.emit('message', d); if (this.onmessage)
            this.onmessage({ data: d, target: this }); });
        h.on('close', (code, reason) => {
            this.emit('close', code, reason);
            if (this.onclose)
                this.onclose({ code: code, reason: reason, wasClean: code === 1000 });
        });
        h.on('error', (m) => {
            const err = new Error(m);
            this.emit('error', err);
            if (this.onerror)
                this.onerror({ error: err, message: m });
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
        if (opts && opts.port)
            h.listen(opts.port | 0, opts.host || '');
    }
    close(cb) { if (cb)
        this.on('close', cb); this._h.close(); }
    address() { const a = this._h.address; return (a && a.port) ? a : null; }
}
const ws = {
    WebSocket: WebSocketExt,
    WebSocketServer: WebSocketServer,
    Server: WebSocketServer,
    createServer(opts, cb) {
        const s = new WebSocketServer(opts);
        if (cb)
            s.on('listening', cb);
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
        if (typeof a === 'object' && a !== null) {
            qos = a.qos || 0;
            retain = !!a.retain;
            cb = b;
        }
        else if (typeof a === 'function')
            cb = a;
        this._h.publish(topic, toBytes(message, null), qos | 0, retain);
        if (cb)
            cb();
        return this;
    }
    subscribe(topic, a, b) {
        let qos = 0, cb = null;
        if (typeof a === 'object' && a !== null) {
            qos = a.qos || 0;
            cb = b;
        }
        else if (typeof a === 'function')
            cb = a;
        const list = Array.isArray(topic) ? topic : [topic];
        for (let i = 0; i < list.length; i++)
            this._h.subscribe(list[i], qos | 0);
        if (cb)
            cb(null, list.map(function (t) { return { topic: t, qos: qos | 0 }; }));
        return this;
    }
    unsubscribe(topic, cb) {
        const list = Array.isArray(topic) ? topic : [topic];
        for (let i = 0; i < list.length; i++)
            this._h.unsubscribe(list[i]);
        if (cb)
            cb();
        return this;
    }
    end(force, cb) {
        if (typeof force === 'function')
            force();
        else if (cb)
            cb();
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
        if (!init)
            return;
        if (typeof init === 'string') {
            const s = init.charAt(0) === '?' ? init.slice(1) : init;
            if (!s)
                return;
            const parts = s.split('&');
            for (let i = 0; i < parts.length; i++) {
                if (!parts[i])
                    continue;
                const p = parts[i].indexOf('=');
                if (p < 0)
                    this._m[decodeURIComponent(parts[i])] = '';
                else
                    this._m[decodeURIComponent(parts[i].slice(0, p))] = decodeURIComponent(parts[i].slice(p + 1));
            }
        }
        else {
            for (const k in init)
                this._m[k] = String(init[k]);
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
    values() { const r = []; for (const k in this._m)
        r.push(this._m[k]); return r; }
    entries() { const r = []; for (const k in this._m)
        r.push([k, this._m[k]]); return r; }
    forEach(cb) { for (const k in this._m)
        cb(this._m[k], k, this); }
    toString() {
        const r = [];
        for (const k in this._m)
            r.push(encodeURIComponent(k) + '=' + encodeURIComponent(this._m[k]));
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
// ------------------------------------------------------------ 全局(供全局脚本直接访问; import 用法由 export 提供)
globalThis.net = net;
globalThis.dgram = dgram;
globalThis.http = http;
globalThis.mqtt = mqtt;
globalThis.url = url;
globalThis.os = os;
globalThis.ws = ws;
export { net, dgram, http, mqtt, ws, url, os };
