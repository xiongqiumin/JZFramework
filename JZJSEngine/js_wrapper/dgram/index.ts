/**
 * @jz/dgram —— Node `dgram` 模块(UDP)。
 * 包装 __jz.dgram 原子 API。默认导出 = 模块对象, 具名导出 = 各成员。
 */
'use strict';

import { EventEmitter } from 'events/dist';
import { fromBytes, toBytes } from 'internal/dist';

declare const __jz: any;
declare function setTimeout(cb: (...a: any[]) => void, ms?: number): any;

export class Socket extends EventEmitter {
    _h: any;
    _enc: any;
    constructor(h: any) {
        super();
        this._h = h;
        this._enc = null;
        h.on('message', (b: any, rinfo: any) => {
            this.emit('message', this._enc ? fromBytes(b, this._enc) : b, rinfo);
        });
        h.on('listening', () => this.emit('listening'));
        h.on('close', () => setTimeout(() => this.emit('close'), 0));
        h.on('error', (m: any) => this.emit('error', new Error(m)));
    }
    bind(a: any, b: any, c: any) {
        let port = 0, cb: any = null;
        if (a && typeof a === 'object') { port = a.port || 0; cb = b; }
        else { port = a || 0; cb = (typeof b === 'function') ? b : c; }
        if (cb) this.on('listening', cb);
        this._h.bind(port | 0);
        return this;
    }
    send(msg: any, a: any, b: any, c: any, d: any, e: any) {
        let port: any, addr: any, cb: any = null, data = toBytes(msg, null);
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
    close(cb: any) { if (cb) this.on('close', cb); this._h.close(); return this; }
    address() { return this._h.address; }
    setEncoding(enc: any) { this._enc = enc || null; return this; }
    /**
     * 让这个 socket **不再**把进程留住(ref = 反过来)。与 Node 同义:
     * unref 之后照样收发包, 只是宿主不再因为它留在事件循环里(未 bind 时调用也记住该状态)。
     */
    ref() { this._h.ref(); return this; }
    unref() { this._h.unref(); return this; }
}

export function createSocket(type: any, listener: any) {
    if (type !== 'udp4') throw new TypeError('only udp4 is supported');
    const s = new Socket(__jz.dgram.createSocket());
    if (listener) s.on('message', listener);
    return s;
}

const dgram = { Socket, createSocket };
export default dgram;
