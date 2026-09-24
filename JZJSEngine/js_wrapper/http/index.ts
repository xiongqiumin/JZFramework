/**
 * @jz/http —— Node `http` 模块(客户端 + 服务端)。
 *
 * 包装 __jz.http 原子 API。默认导出 = 模块对象(对齐 Node 的 http 对象),
 * 具名导出 = 各成员 ⇒ `import http from 'http'` 与 `import { createServer } from 'http'` 都能用。
 *
 * 本模块顺带实现并挂到 globalThis 的 **Web 全局**: fetch / Headers / Response
 * (对齐 Node 18+/浏览器: 启动即有, 无需 import; 类型见共享声明 types/fetch.d.ts)。
 */
'use strict';

import { EventEmitter } from 'events/dist';
import { fromBytes, isEnc, toBytes } from 'internal/dist';

declare const __jz: any;

// Headers/Response 只做"Web 全局"实现(挂 globalThis), **不作为 http 模块的具名导出**
// —— 类型侧也一样(共享 types/fetch.d.ts 声明它们是全局), 避免"运行时有、types 无"的漂移。
class Headers {
    _m: any;
    constructor(init: any) {
        this._m = Object.create(null);
        if (init) {
            if (typeof init.forEach === 'function' && !Array.isArray(init)) {
                init.forEach((v: any, k: any) => { this._m[String(k).toLowerCase()] = String(v); });
            } else {
                for (const k in init) this._m[String(k).toLowerCase()] = String(init[k]);
            }
        }
    }
    get(n: any) { const v = this._m[String(n).toLowerCase()]; return v === undefined ? null : v; }
    has(n: any) { return this._m[String(n).toLowerCase()] !== undefined; }
    set(n: any, v: any) { this._m[String(n).toLowerCase()] = String(v); }
    append(n: any, v: any) {
        const k = String(n).toLowerCase();
        this._m[k] = (this._m[k] === undefined) ? String(v) : this._m[k] + ', ' + String(v);
    }
    delete(n: any) { delete this._m[String(n).toLowerCase()]; }
    keys() { return Object.keys(this._m); }
    values() { const r: any[] = []; for (const k in this._m) r.push(this._m[k]); return r; }
    entries() { const r: any[] = []; for (const k in this._m) r.push([k, this._m[k]]); return r; }
    forEach(cb: any) { for (const k in this._m) cb(this._m[k], k, this); }
    toJSON() { const r: any = {}; for (const k in this._m) r[k] = this._m[k]; return r; }
}
(globalThis as any).Headers = Headers;

// Node 的 http 请求/响应头是**普通对象**(按小写 key 索引), 不是 Web Headers 实例 ——
// 这里转成普通对象, 这样 `req.headers['content-type']` 与 Node 一致(Web 的 fetch/Response
// 仍用上面的 Headers 类, 那是标准, 不混)。
function plainHeaders(h: any): any {
    const o: any = {};
    if (h) for (const k in h) o[String(k).toLowerCase()] = h[k];
    return o;
}

// ---- 客户端 ----
export class IncomingMessage extends EventEmitter {
    statusCode: any;
    statusMessage: any;
    headers: any;
    httpVersion: any;
    url: any;
    _body: any;
    _enc: any;
    constructor(info: any) {
        super();
        this.statusCode = info.status;
        this.statusMessage = info.statusText;
        this.headers = plainHeaders(info.headers);
        this.httpVersion = '1.1';
        this.url = info.url;
        this._body = info.body;
        this._enc = null;
    }
    setEncoding(enc: any) { this._enc = enc || null; return this; }
    _start() {
        this.emit('data', this._enc ? fromBytes(this._body, this._enc) : this._body);
        this.emit('end');
    }
}

export class ClientRequest extends EventEmitter {
    _opts: any;
    _headers: any;
    _body: any;
    _sent: boolean;
    constructor(opts: any) {
        super();
        this._opts = opts || {};
        this._headers = {};
        if (this._opts.headers) for (const k in this._opts.headers) this._headers[k] = this._opts.headers[k];
        this._body = new Uint8Array(0);
        this._sent = false;
    }
    setHeader(k: any, v: any) { this._headers[k] = String(v); }
    getHeader(k: any) { return this._headers[k]; }
    removeHeader(k: any) { delete this._headers[k]; }
    write(chunk: any, enc?: any, cb?: any) {
        const b = toBytes(chunk, isEnc(enc) ? enc : null);
        const n = new Uint8Array(this._body.length + b.length);
        n.set(this._body, 0); n.set(b, this._body.length);
        this._body = n;
        if (typeof enc === 'function') enc(); else if (cb) cb();
        return true;
    }
    end(data: any, enc: any, cb: any) {
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
    setTimeout(ms: any, cb: any) { if (this._opts) this._opts.timeout = ms; if (cb) this.on('timeout', cb); return this; }
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
        __jz.http.request(req, this._body).then(function (info: any) {
            const res = new IncomingMessage(info);
            self.emit('response', res);
            res._start();
        }, function (err: any) {
            self.emit('error', err instanceof Error ? err : new Error(String(err)));
        });
    }
}

function httpRequest(url: any, options: any, cb: any) {
    let opts: any = {}, callback: any = null;
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
        req._send();
    }
    return req;
}

// ---- fetch / Response(Web 全局, 见文件头说明) ----
class Response {
    status: any;
    statusText: any;
    ok: any;
    url: any;
    headers: any;
    _body: any;
    bodyUsed: any;
    constructor(info: any) {
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
(globalThis as any).Response = Response;

(globalThis as any).fetch = function (url: any, init: any) {
    init = init || {};
    let body = new Uint8Array(0);
    if (init.body !== undefined && init.body !== null) body = toBytes(init.body, null);
    const req = {
        url: String(url),
        method: init.method || (body.length ? 'POST' : 'GET'),
        headers: init.headers || {},
        timeout: init.timeout || 0
    };
    return __jz.http.request(req, body).then(function (info: any) { return new Response(info); });
};

// ---- 服务端 ----
export class ServerRequest extends EventEmitter {
    method: any;
    url: any;
    headers: any;
    httpVersion: any;
    remoteAddress: any;
    _body: any;
    _enc: any;
    constructor(info: any) {
        super();
        this.method = info.method;
        this.url = info.url;
        this.headers = plainHeaders(info.headers);
        this.httpVersion = '1.1';
        this.remoteAddress = info.remoteAddress || '';
        this._body = info.body;
        this._enc = null;
    }
    setEncoding(enc: any) { this._enc = enc || null; return this; }
    get text() { return fromBytes(this._body, this._enc); }
    get body() { return this._body; }
    _start() {
        this.emit('data', this._enc ? fromBytes(this._body, this._enc) : this._body);
        this.emit('end');
    }
}

export class ServerResponse extends EventEmitter {
    _h: any;
    _ended: boolean;
    _headers: any;
    constructor(h: any) {
        super();
        this._h = h;
        this._ended = false;
        this._headers = {};
    }
    get statusCode() { return this._h.statusCode; }
    set statusCode(v: any) { this._h.statusCode = v | 0; }
    get finished() { return this._ended; }
    setHeader(k: any, v: any) {
        this._headers[String(k).toLowerCase()] = String(v);
        this._h.setHeader(String(k), String(v));
        return this;
    }
    getHeader(k: any) { return this._headers[String(k).toLowerCase()]; }
    removeHeader(k: any) {
        delete this._headers[String(k).toLowerCase()];
        // C++ JZJSHttpResponse 暂无 removeHeader: JS 侧移除以保证 getHeader 一致;
        // 已通过 writeHead 写入且未再改动的头仍会发送(罕见路径)。
        return this;
    }
    writeHead(status: any, headers: any) {
        if (headers) for (const k in headers) this._headers[String(k).toLowerCase()] = String(headers[k]);
        this._h.writeHead(status | 0, headers || {});
        return this;
    }
    write(chunk: any, enc?: any, cb?: any) {
        if (this._ended) return false;
        this._h.write(toBytes(chunk, isEnc(enc) ? enc : null));
        if (typeof enc === 'function') enc(); else if (cb) cb();
        return true;
    }
    end(data: any, enc: any, cb: any) {
        if (this._ended) return this;
        const hasData = data !== undefined && data !== null && typeof data !== 'function';
        this._h.end(hasData ? toBytes(data, isEnc(enc) ? enc : null) : new Uint8Array(0));
        this._ended = true;
        if (typeof data === 'function') data();
        else if (typeof enc === 'function') enc();
        else if (cb) cb();
        this.emit('finish');
        return this;
    }
}

export class Server extends EventEmitter {
    _h: any;
    constructor(h: any) {
        super();
        this._h = h;
        h.on('request', (rq: any, rs: any) => {
            const req = new ServerRequest(rq);
            const res = new ServerResponse(rs);
            this.emit('request', req, res);
            req._start();
        });
        h.on('listening', () => this.emit('listening'));
        h.on('close', () => this.emit('close'));
        h.on('error', (m: any) => this.emit('error', new Error(m)));
    }
    get listening() { return this._h.listening; }
    address() { return this._h.address; }
    listen(a: any, b?: any, c?: any) {
        let port = 0, host = '', cb: any = null;
        if (typeof a === 'number') {
            port = a;
            if (typeof b === 'string') host = b; else cb = b;
            if (typeof c === 'function') cb = c;
        } else if (a && typeof a === 'object') {
            port = a.port | 0;
            host = a.host || '';
            cb = b;
        }
        if (cb) this.on('listening', cb);
        this._h.listen(port, host);
        return this;
    }
    close(cb: any) {
        if (cb) this.on('close', cb);
        this._h.close();
        return this;
    }
}

export function createServer(opts: any, listener: any) {
    const s = new Server(__jz.http.createServer());
    let cb = listener;
    if (typeof opts === 'function') cb = opts;
    if (cb) s.on('request', cb);
    return s;
}

export function request(url: any, options: any, cb: any) { return httpRequest(url, options, cb); }

export function get(url: any, options: any, cb: any) {
    const req = httpRequest(url, options, cb);
    req._opts.method = 'GET';
    req._send();
    return req;
}

export const METHODS = ['GET', 'POST'];

export const STATUS_CODES: any = {
    100: 'Continue', 101: 'Switching Protocols', 200: 'OK', 201: 'Created',
    202: 'Accepted', 204: 'No Content', 206: 'Partial Content',
    301: 'Moved Permanently', 302: 'Found', 304: 'Not Modified',
    400: 'Bad Request', 401: 'Unauthorized', 403: 'Forbidden', 404: 'Not Found',
    405: 'Method Not Allowed', 408: 'Request Timeout', 409: 'Conflict',
    413: 'Payload Too Large', 415: 'Unsupported Media Type',
    500: 'Internal Server Error', 501: 'Not Implemented',
    502: 'Bad Gateway', 503: 'Service Unavailable', 504: 'Gateway Timeout'
};

const http = {
    IncomingMessage, ClientRequest, ServerRequest, ServerResponse, Server,
    createServer, request, get, METHODS, STATUS_CODES
};
export default http;
