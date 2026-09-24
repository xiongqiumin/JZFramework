/**
 * @jz/events —— Node `events` 模块 (EventEmitter) 的运行时实现。
 *
 * 全框架**唯一一份** EventEmitter(以前在 net 包里, 现在独立成同名包):
 *   - 其它包要用请 `import { EventEmitter } from 'events/dist'`, 不要再各自抄一份;
 *   - 默认导出就是 EventEmitter 类(对齐 Node: `import EventEmitter from 'events'`),
 *     具名导入 `import { EventEmitter } from 'events'` 同样可用;
 *   - 同时挂到 globalThis(对齐 Node 的全局 ee; 但明确依赖请用 import)。
 *
 * 与 Node 一致(✅ 已对齐): 没有 'error' 监听器时 `emit('error', err)` 会**抛异常**
 * (Node 的 Unhandled 'error' event 语义) —— 错误不会被静默吞掉。
 * 当初没抛是担心抛在引擎事件派发里难处理; 实际上引擎对回调异常有顶层兜底
 * (按"未捕获异常"上报失败, 跟 Node 进程崩一个效果), 所以现在严格照 Node 来。
 * 不想让错误冒泡: 自己 `on('error', ...)` 接住即可(与 Node 完全相同)。
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
            if (e === 'error') {
                // Node 语义: 没有 'error' 监听器必须抛, 否则错误被静默吞掉(初学者最易踩的坑)。
                // 引擎对回调异常有顶层兜底, 抛出来会按"未捕获异常"上报失败 —— 与 Node 进程崩一致。
                // 消息格式对齐 Node v26: Error 实例直接原样抛出; 其它参数抛 `Unhandled error. (<inspect>)`
                // (字符串用单引号包, 与 Node 的 util.inspect 行为一致)。
                if (args.length > 0 && args[0] instanceof Error) throw args[0];
                const a = args.length > 0 ? args[0] : undefined;
                const s = a === undefined ? '' : (typeof a === 'string' ? "'" + a + "'" : String(a));
                throw new Error('Unhandled error.' + (s ? ' (' + s + ')' : ''));
            }
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
