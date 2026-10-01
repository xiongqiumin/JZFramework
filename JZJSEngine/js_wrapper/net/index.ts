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
    _ended: boolean;          // 已 end()(半关闭: 写方向结束, 读方向继续)
    _destroyed: boolean;      // 已 destroy()(: 之后 write 报错、事件按 Node 收尾)
    _paused: boolean;         // pause() 之后: 'data' 先攒在 _pauseBuf, resume() 时补发
    _pauseBuf: any[];
    constructor(h?: any) {
        super();
        this._ended = false;
        this._destroyed = false;
        this._paused = false;
        this._pauseBuf = [];
        // Node: new Socket() works without args. A native handle has an .on(); a
        // plain options object does not, so create the handle in that case.
        this._h = (h && typeof h.on === 'function') ? h : __jz.net.createSocket();
        this._enc = null;
        this._timeout = 0;
        this._timer = 0;
        this._h.on('connect', () => this.emit('connect'));
        this._h.on('data', (b: any) => {
            if (this._timeout > 0) this._armTimeout();
            const chunk = this._enc ? fromBytes(b, this._enc) : b;
            if (this._paused) this._pauseBuf.push(chunk);   // pause() 期间先攒着(Node 语义: 不丢数据)
            else this.emit('data', chunk);
        });
        this._h.on('end', () => this.emit('end'));
        this._h.on('close', () => { this._disarm(); this.emit('close'); });
        this._h.on('error', (m: any) => this.emit('error', new Error(m)));
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
        // Node: connect() without a port throws ERR_MISSING_ARGS synchronously.
        if (opts.port == null) {
            const e: any = new Error("net.connect: 'options.port' is required");
            e.code = 'ERR_MISSING_ARGS';
            throw e;
        }
        // Node: a second connect() on an already-connecting/connected socket is a
        // no-op. The native layer crashes on re-entrant connectToHost, so skip it.
        // Drive the check from the underlying C++ state (0=Unconnected,1=Connecting,2=Connected,3=destroyed).
        if (this._h.state === 1 || this._h.state === 2) return this;
        if (cb) this.on('connect', cb);
        this._armTimeout();
        this._h.connectTo(opts.port | 0, opts.host || '127.0.0.1');
        return this;
    }
    write(data: any, enc: any, cb: any) {
        if (this._destroyed) {
            // 对齐 Node: destroy() 之后写入 → ERR_STREAM_DESTROYED
            const e: any = new Error('Cannot call write after a stream was destroyed');
            e.code = 'ERR_STREAM_DESTROYED';
            const done = (typeof enc === 'function') ? enc : cb;
            setTimeout(() => this.emit('error', e), 0);
            if (done) done(e);
            return false;
        }
        if (this._ended) {
            // 对齐 Node: end()(半关闭)之后写入 → ERR_STREAM_WRITE_AFTER_END, 异步 emit 'error'
            const e: any = new Error('write after end');
            e.code = 'ERR_STREAM_WRITE_AFTER_END';
            const done = (typeof enc === 'function') ? enc : cb;
            setTimeout(() => this.emit('error', e), 0);
            if (done) done(e);
            return false;
        }
        const ok = this._h.write(toBytes(data, isEnc(enc) ? enc : null));
        if (typeof enc === 'function') enc();
        else if (cb) cb();
        return ok;
    }
    /**
     * 半关闭(对齐 Node 的 socket.end()): 只关写方向 —— 不再往对端发数据, 但**本端仍能收数据**
     * (Node 里 `write(x); end();` 之后照样能收到对端回包)。想整个断开用 destroy()。
     * 'finish' 在写侧结束时异步发(Node 的 Writable 语义), end(cb) 的回调挂在 'finish' 上。
     */
    end(data: any, enc: any, cb: any) {
        if (this._ended || this._destroyed) return this;
        if (data !== undefined && data !== null) this._h.write(toBytes(data, isEnc(enc) ? enc : null));
        this._ended = true;
        this._h.end();
        const done = (typeof data === 'function') ? data : ((typeof enc === 'function') ? enc : cb);
        setTimeout(() => { this.emit('finish'); if (done) done(); }, 0);
        return this;
    }
    /** 对齐 Node: destroy([error]) —— 出错时先发 'error' 再发 'close'; 幂等 */
    destroy(err?: any) {
        if (this._destroyed) return this;
        this._destroyed = true;
        this._disarm();
        this._h.destroy();
        if (err) setTimeout(() => this.emit('error', err), 0);
        setTimeout(() => this.emit('close'), 0);     // Node: destroy 之后一定会收到一次 'close'
        return this;
    }
    /** 对齐 Node 的 stream 语义: pause() 之后 'data' 先攒住, resume() 时按序补发 */
    pause() { this._paused = true; return this; }
    resume() {
        if (!this._paused) return this;
        this._paused = false;
        const buf = this._pauseBuf;
        this._pauseBuf = [];
        for (const c of buf) this.emit('data', c);
        return this;
    }
    isPaused() { return this._paused; }
    /**
     * Node 的 net.Socket 会带上本端/对端地址族。本引擎只讲 IPv4/IPv6 两种文本形式,
     * 按地址里有没有 ':' 判断(与 Node 的取值一致: 'IPv4' / 'IPv6')。
     */
    static _familyOf(addr: any) {
        if (!addr || typeof addr !== 'string') return undefined;
        return addr.indexOf(':') >= 0 ? 'IPv6' : 'IPv4';
    }
    /** 对齐 Node: net.Socket#address() —— 本端 {address, family, port} */
    address() {
        const addr = this._h.localAddress;
        const port = this._h.localPort;
        if (!addr) return null;
        return { address: addr, family: (Socket as any)._familyOf(addr), port: port | 0 };
    }
    /**
     * ⚠ 本引擎基于 mongoose, 没有暴露 TCP_NODELAY / SO_KEEPALIVE 的按连接开关
     * (mongoose 自己在建连/accept 时处理)。这里按 Node 的形状接受并忽略,
     * 免得 Node 代码因此报错 —— 明确是 no-op, 不假装成功。
     */
    setNoDelay(_noDelay?: any, cb?: any) { if (typeof _noDelay === 'function') _noDelay(); else if (cb) cb(); return this; }
    setKeepAlive(_enable?: any, _initialDelay?: any, cb?: any) {
        if (typeof _enable === 'function') _enable();
        else if (typeof _initialDelay === 'function') _initialDelay();
        else if (cb) cb();
        return this;
    }
    setTimeout(ms: any, cb: any) { this._timeout = ms | 0; if (cb) this.on('timeout', cb); this._armTimeout(); return this; }
    setEncoding(enc: any) { this._enc = enc || null; return this; }
    /**
     * 让这个 socket **不再**把进程留住(ref = 反过来)。与 Node 同义:
     * unref 之后 socket 照常收发, 只是宿主不再因为它而留在事件循环里;
     * 连不上/已断开时调用也有效(unref 状态会留着)。
     */
    ref() { this._h.ref(); return this; }
    unref() { this._h.unref(); return this; }
    get remoteAddress() { return this._h.remoteAddress; }
    get remotePort() { return this._h.remotePort; }
    get remoteFamily() { return (Socket as any)._familyOf(this._h.remoteAddress); }
    get localAddress() { return this._h.localAddress; }
    get localPort() { return this._h.localPort; }
    get localFamily() { return (Socket as any)._familyOf(this._h.localAddress); }
    get bytesRead() { return this._h.bytesRead; }
    get bytesWritten() { return this._h.bytesWritten; }
    get destroyed() { return this._destroyed || this._h.destroyed; }
    get writableEnded() { return this._ended; }
    /** 对齐 Node: 连接还没建立时为 true */
    get pending() { return this._h.state === 1; }
    // C++ SocketState: 0=Unconnected, 1=Connecting, 2=Connected, 3=destroyed.
    get readyState() {
        const s = this._h.state;
        return s === 2 ? 'open' : (s === 1 ? 'opening' : 'closed');
    }
    /** true while a connect() is in progress but not yet established (Node parity) */
    get connecting() { return this._h.state === 1; }
}

export class Server extends EventEmitter {
    _h: any;
    maxConnections: number;
    _conns: Socket[];
    constructor(h: any) {
        super();
        this._h = h;
        this.maxConnections = 0;
        this._conns = [];
        h.on('connection', (s: any) => {
            const sock = new Socket(s);
            // 对齐 Node: 到达 maxConnections 后**直接丢掉**新连接(不发 'connection')
            if (this.maxConnections > 0 && this._conns.length >= this.maxConnections) {
                try { sock.destroy(); } catch (e) { /* ignore */ }
                return;
            }
            this._conns.push(sock);
            sock.on('close', () => {
                const i = this._conns.indexOf(sock);
                if (i >= 0) this._conns.splice(i, 1);
            });
            this.emit('connection', sock);
        });
        h.on('listening', () => this.emit('listening'));
        h.on('close', () => setTimeout(() => this.emit('close'), 0));
        // 对齐 Node: 监听失败要发 'error'(原生层 listen 失败时发, 见 JZJSNetServer::listen)。
        // Node 里这类错误带 err.code(Node 代码普遍按 code 分支, 如 EADDRINUSE), 底层只给文本,
        // 这里按 "listen EADDRINUSE: ..." 的形状把 code 摘出来补上。
        h.on('error', (m: any) => {
            const err = (m instanceof Error) ? m : new Error(String(m));
            if (!(err as any).code) {
                const mm = /^(?:listen )?([A-Z]+)[:]/.exec(String((m && m.message) || m));
                if (mm) (err as any).code = mm[1];
            }
            this.emit('error', err);
        });
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
    /** 对齐 Node: 当前活跃连接数(Node 的回调是 (err, count)) */
    getConnections(cb: any) {
        const n = this._conns.length;
        setTimeout(() => { if (cb) cb(null, n); }, 0);
        return this;
    }
    /** 对齐 Node(18+): 关掉本服务 accept 出来的所有连接(不影响监听本身) */
    closeAllConnections() {
        const list = this._conns.slice();
        for (const s of list) { try { s.destroy(); } catch (e) { /* ignore */ } }
        this._conns = [];
        return this;
    }
    /**
     * ⚠ Node(18+) 的 closeIdleConnections 只关"当前空闲"的连接; 本引擎拿不到
     * "这条连接是否正在处理请求"的信息 ⇒ 按 Node 的 closeAllConnections 处理(不减语义)。
     */
    closeIdleConnections() { return this.closeAllConnections(); }
    /** 让监听中的 server 不再吊住进程(与 Node 同义; 之后可以再 ref() 回来) */
    ref() { this._h.ref(); return this; }
    unref() { this._h.unref(); return this; }
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
    // Node: createConnection()/connect() without a port throws ERR_MISSING_ARGS.
    if (opts.port == null) {
        const e: any = new Error("net.createConnection: 'options.port' is required");
        e.code = 'ERR_MISSING_ARGS';
        throw e;
    }
    const s = new Socket(__jz.net.createSocket());
    if (cb) s.on('connect', cb);
    s.connect(opts);
    return s;
}

/** createConnection 的别名(Node 同名) */
export const connect = createConnection;

/** 对齐 Node 的 net.isIPv4: 点分四段、每段 0-255、不允许前导零 */
export function isIPv4(s: any) {
    if (typeof s !== 'string') return false;
    const m = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/.exec(s);
    if (!m) return false;
    for (let i = 1; i <= 4; i++) {
        if (m[i].length > 1 && m[i][0] === '0') return false;
        if (+m[i] > 255) return false;
    }
    return true;
}

/**
 * 对齐 Node 的 net.isIPv6: 8 组(或 '::' 缩写后不足 8 组)十六进制, 末段允许内嵌 IPv4。
 * 只做语法判断(不做 DNS/地址归属查询), 与 Node 的行为一致。
 */
export function isIPv6(s: any) {
    if (typeof s !== 'string' || s.indexOf(':') < 0) return false;
    if (!/^[0-9a-fA-F:.]+$/.test(s)) return false;
    const dbl = s.split('::');
    if (dbl.length > 2) return false;
    let groups: string[] = [];
    for (const part of dbl) if (part.length) groups = groups.concat(part.split(':'));
    if (groups.length === 0 || groups.some((g) => g.length === 0)) return false;
    let count = 0;
    for (let i = 0; i < groups.length; i++) {
        const g = groups[i];
        if (g.indexOf('.') >= 0) {                       // 内嵌 IPv4 只能在最后一段
            if (i !== groups.length - 1 || !isIPv4(g)) return false;
            count += 2;
        } else if (/^[0-9a-fA-F]{1,4}$/.test(g)) {
            count += 1;
        } else {
            return false;
        }
    }
    return dbl.length === 2 ? count < 8 : count === 8;
}

/** 对齐 Node 的 net.isIP: 4 = IPv4, 6 = IPv6, 0 = 都不是 */
export function isIP(s: any) { return isIPv4(s) ? 4 : (isIPv6(s) ? 6 : 0); }

const net = { Socket, Server, createServer, createConnection, connect, isIP, isIPv4, isIPv6 };
export default net;
