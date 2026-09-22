/**
 * @jz/events —— Node `events` 模块 (EventEmitter) 的运行时实现。
 *
 * 全框架**唯一一份** EventEmitter(以前在 net 包里, 现在独立成同名包):
 *   - 其它包要用请 `import { EventEmitter } from 'events/dist'`, 不要再各自抄一份;
 *   - 默认导出就是 EventEmitter 类(对齐 Node: `import EventEmitter from 'events'`),
 *     具名导入 `import { EventEmitter } from 'events'` 同样可用;
 *   - 同时挂到 globalThis(对齐 Node 的全局 ee; 但明确依赖请用 import)。
 *
 * 与 Node 的差异: 没有 'error' 监听器时 Node 会 throw, 本实现改为 console.error 上报,
 * 不会抛出未捕获异常(见 shared types/events.d.ts 的注释)。
 */
'use strict';

declare var console: { error(...a: any[]): void; log(...a: any[]): void; warn(...a: any[]): void };

export class EventEmitter {
    _ev: any;
    constructor() { this._ev = Object.create(null); }
    on(e: string, l: Function): any {
        if (!this._ev[e]) this._ev[e] = [];
        this._ev[e].push(l);
        return this;
    }
    once(e: string, l: Function): any {
        const self = this;
        function w(this: any) { self.off(e, w); (l as any).apply(self, arguments); }
        (w as any).__orig = l;
        return this.on(e, w);
    }
    off(e: string, l?: Function): any {
        const a = this._ev[e];
        if (!a) return this;
        if (!l) { delete this._ev[e]; return this; }
        for (let i = a.length - 1; i >= 0; i--) {
            if (a[i] === l || (a[i] as any).__orig === l) a.splice(i, 1);
        }
        if (!a.length) delete this._ev[e];
        return this;
    }
    removeListener(e: string, l: Function): any { return this.off(e, l); }
    removeAllListeners(e?: string): any {
        if (e) delete this._ev[e]; else this._ev = Object.create(null);
        return this;
    }
    listenerCount(e: string): number { return this._ev[e] ? this._ev[e].length : 0; }
    listeners(e: string): Function[] { return (this._ev[e] || []).slice(); }   // Node 同名 API: 返回副本
    eventNames(): string[] { return Object.keys(this._ev); }
    emit(e: string, ...args: any[]): boolean {
        const list = this._ev[e];
        if (!list || !list.length) {
            if (e === 'error') console.error('Unhandled error:', args[0]);
            return false;
        }
        const arr = list.slice();
        for (let i = 0; i < arr.length; i++) {
            try { (arr[i] as any).apply(this, args); }
            catch (err) { console.error('listener error:', err); }
        }
        return true;
    }
}

(globalThis as any).EventEmitter = EventEmitter;

export default EventEmitter;
