/**
 * @jz/net —— Node `net` 模块(TCP 客户端 / 服务端)。
 *
 * 包装引擎注入的 __jz.net 原子 API。默认导出 = 模块对象(对齐 Node 的 net 对象),
 * 具名导出 = 各成员 ⇒ `import net from 'net'` 与 `import { Socket } from 'net'` 都能用。
 *
 * 依赖: events(EventEmitter) / internal(toBytes/fromBytes/isEnc)。
 * 本包只做 net 一个模块(os/url/ws/http/dgram/mqtt/events 都独立成同名包了)。
 */
'use strict';

import { EventEmitter } from 'events/dist';
import { fromBytes, isEnc, toBytes } from 'internal/dist';

declare const __jz: any;
declare function setTimeout(cb: (...a: any[]) => void, ms?: number): any;
declare function clearTimeout(id: any): void;

export class Socket extends EventEmitter {
    _h: any;
    _enc: any;
    _timeout: number;
    _timer: any;
    constructor(h: any) {
        super();
        this._h = h;
        this._enc = null;
        this._timeout = 0;
        this._timer = 0;
        h.on('connect', () => this.emit('connect'));
        h.on('data', (b: any) => {
            if (this._timeout > 0) this._armTimeout();
            this.emit('data', this._enc ? fromBytes(b, this._enc) : b);
        });
        h.on('end', () => this.emit('end'));
        h.on('close', () => { this._disarm(); this.emit('close'); });
        h.on('error', (m: any) => this.emit('error', new Error(m)));
    }
    _armTimeout() {
        this._disarm();
        if (this._timeout > 0) this._timer = setTimeout(() => this.emit('timeout'), this._timeout);
    }
    _disarm() { if (this._timer) { clearTimeout(this._timer); this._timer = 0; } }
    connect(a: any, b?: any, c?: any) {
        let opts: any = {};
        let cb: any = null;
        if (typeof a === 'number') { opts = { port: a, host: b || '127.0.0.1' }; cb = c; }
        else { opts = a || {}; cb = b; }
        if (cb) this.on('connect', cb);
        this._armTimeout();
        this._h.connectTo(opts.port | 0, opts.host || '127.0.0.1');
        return this;
    }
    write(data: any, enc: any, cb: any) {
        const ok = this._h.write(toBytes(data, isEnc(enc) ? enc : null));
        if (typeof enc === 'function') enc();
        else if (cb) cb();
        return ok;
    }
    end(data: any, enc: any, cb: any) {
        if (data !== undefined && data !== null) this._h.write(toBytes(data, isEnc(enc) ? enc : null));
        this._h.end();
        if (typeof data === 'function') data();
        else if (typeof enc === 'function') enc();
        else if (cb) cb();
        return this;
    }
    destroy() { this._disarm(); this._h.destroy(); return this; }
    setTimeout(ms: any, cb: any) { this._timeout = ms | 0; if (cb) this.on('timeout', cb); this._armTimeout(); return this; }
    setEncoding(enc: any) { this._enc = enc || null; return this; }
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

export class Server extends EventEmitter {
    _h: any;
    maxConnections: number;
    constructor(h: any) {
        super();
        this._h = h;
        this.maxConnections = 0;
        h.on('connection', (s: any) => this.emit('connection', new Socket(s)));
        h.on('listening', () => this.emit('listening'));
        h.on('close', () => setTimeout(() => this.emit('close'), 0));
    }
    listen(a: any, b?: any, c?: any) {
        let port = 0, host = '', cb: any = null;
        if (a && typeof a === 'object') { port = a.port || 0; host = a.host || ''; cb = b; }
        else { port = a || 0; if (typeof b === 'string') { host = b; cb = c; } else cb = b; }
        if (cb) this.on('listening', cb);
        this._h.listen(port | 0, host);
        return this;
    }
    close(cb: any) { if (cb) this.on('close', cb); this._h.close(); return this; }
    address() { const a = this._h.address; return (a && a.port) ? a : null; }
    get listening() { return this._h.listening; }
}

export function createServer(a: any, b: any) {
    const cb = (typeof a === 'function') ? a : b;
    const opts = (a && typeof a === 'object') ? a : {};
    const s = new Server(__jz.net.createServer());
    if (cb) s.on('connection', cb);
    if (opts.port) s.listen(opts.port, opts.host);
    return s;
}

export function createConnection(a: any, b: any, c: any) {
    let opts: any, cb: any = null;
    if (typeof a === 'number') { opts = { port: a, host: b || '127.0.0.1' }; cb = c; }
    else { opts = a || {}; cb = b; }
    const s = new Socket(__jz.net.createSocket());
    if (cb) s.on('connect', cb);
    s.connect(opts);
    return s;
}

/** createConnection 的别名(Node 同名) */
export const connect = createConnection;

const net = { Socket, Server, createServer, createConnection, connect };
export default net;
