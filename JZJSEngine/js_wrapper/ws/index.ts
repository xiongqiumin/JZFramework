/**
 * @jz/ws —— Node/WHATWG 的 WebSocket 封装(客户端 + 服务端)。
 *
 * ‼ 客户端类 WebSocket 是 **Web 标准全局**(对齐浏览器/Node 18+: 启动即有),
 *   本模块装载时把它挂到 globalThis; 也作为具名导出暴露。
 * 默认导出 = 模块对象(WebSocket / WebSocketServer / Server / createServer)。
 */
'use strict';

import { EventEmitter } from 'events/dist';
import { toBytes } from 'internal/dist';

declare const __jz: any;
declare function setTimeout(cb: (...a: any[]) => void, ms?: number): any;

export class WebSocket extends EventEmitter {
    _h: any;
    onopen: any;
    onmessage: any;
    onclose: any;
    onerror: any;
    constructor(arg: any, protocols?: any, opts?: any) {
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
        h.on('open', () => { this.emit('open'); if (this.onopen) this.onopen({ target: this }); });
        h.on('message', (d: any) => { this.emit('message', d); if (this.onmessage) this.onmessage({ data: d, target: this }); });
        h.on('close', (code: any, reason: any) => {
            this.emit('close', code, reason);
            if (this.onclose) this.onclose({ code: code, reason: reason, wasClean: code === 1000 });
        });
        h.on('error', (m: any) => {
            const err = new Error(m);
            this.emit('error', err);
            if (this.onerror) this.onerror({ error: err, message: m });
        });
    }
    get readyState() { return this._h.state; }
    get url() { return this._h.url; }
    get protocol() { return ''; }
    send(data: any) {
        if (typeof data === 'string')
            this._h.send(data, false);
        else
            this._h.send(toBytes(data, null), true);
    }
    close(code: any, reason: any) { this._h.close(); }
    ping(data: any) { this._h.ping(); }
    /**
     * 让连接**不再**把进程留住(ref = 反过来)。
     * 注: Node 的全局 WebSocket(undici) 没有这个方法(它内部用 socket.unref()); 本引擎的
     * WebSocket 包装的是连接句柄, 所以直接给到这里, 语义与 net.Socket.unref() 一致。
     */
    ref() { this._h.ref(); return this; }
    unref() { this._h.unref(); return this; }
}
(WebSocket as any).CONNECTING = 0;
(WebSocket as any).OPEN = 1;
(WebSocket as any).CLOSING = 2;
(WebSocket as any).CLOSED = 3;
(globalThis as any).WebSocket = WebSocket;

export class WebSocketServer extends EventEmitter {
    _h: any;
    constructor(opts: any) {
        super();
        this._h = __jz.ws.createServer();
        const h = this._h;
        h.on('connection', (s: any) => this.emit('connection', new WebSocket(s)));
        h.on('listening', () => this.emit('listening'));
        h.on('close', () => setTimeout(() => this.emit('close'), 0));
        if (opts && opts.port) h.listen(opts.port | 0, opts.host || '');
    }
    close(cb: any) { if (cb) this.on('close', cb); this._h.close(); }
    address() { const a = this._h.address; return (a && a.port) ? a : null; }
    /** 让监听中的服务不再吊住进程(与 net.Server.unref() 同义) */
    ref() { this._h.ref(); return this; }
    unref() { this._h.unref(); return this; }
}

/** WebSocketServer 的别名(Node 同名) */
export const Server = WebSocketServer;

export function createServer(opts: any, cb: any) {
    const s = new WebSocketServer(opts);
    if (cb) s.on('listening', cb);
    return s;
}

const ws = { WebSocket, WebSocketServer, Server, createServer };
export default ws;
