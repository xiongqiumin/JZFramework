/**
 * @jz/worker_threads —— Node `worker_threads` 兼容层。
 *
 * 引擎的原子能力是私有的 __jz.worker.*(每个引擎**只有一个** onMessage 回调 + 按 id 投递),
 * 这里把它包成 Node 的形状:
 *   主线程: new Worker(entry, { workerData }) / w.on('message'|'exit') / w.postMessage / w.terminate()
 *   worker: parentPort.on('message') / parentPort.postMessage
 *   两侧:   isMainThread / threadId / workerData
 *
 * 与 Node 的差异(引擎语义, 不是 Node 的行为):
 *   - 入口脚本按**运行根**解析(入口 js 所在的目录), 不是"相对当前文件";
 *   - 每个 worker 是独立引擎: 全新空 global, 不继承主线程的全局变量, 只靠消息通信;
 *   - terminate() 是同步的(Node 返回 Promise)。
 */
/// <reference path="../events/types/ambient.d.ts" />
'use strict';

import { EventEmitter } from 'events';

declare const __jz: any;

/** 引擎注入的原生 Worker 类(见 JZJSWorkerModule::registWorkerClass) */
const NativeWorker: any = (globalThis as any).Worker;

const _main: boolean = __jz.worker.isMainThread();
const _workers: Map<number, any> = new Map();   // 主线程: workerId -> Worker 包装实例
let _port: any = null;                          // worker 侧: parentPort
let _dispatch = false;

/** 引擎的 onMessage 是"每引擎一个"的全局回调: 这里注册一次, 再按 fromId 分发到具体对象。 */
function ensureDispatch(): void
{
    if (_dispatch)
        return;
    _dispatch = true;
    __jz.worker.onMessage(function (fromId: number, msg: any): void
    {
        if (!_main)
        {
            if (_port)
                _port.emit('message', msg);
            return;
        }
        const w = _workers.get(fromId);
        if (w)
            w.emit('message', msg);
    });
}

/** MessagePort: 目前只用到 worker 侧的 parentPort(postMessage / on('message') / close)。 */
export class MessagePort
{
    private _ee: EventEmitter;
    private _target: number;

    constructor(target: number)
    {
        this._ee = new EventEmitter();
        this._target = target;
    }

    postMessage(value: any): void { __jz.worker.postTo(this._target, value); }
    close(): void { }
    ref(): void { }
    unref(): void { }

    on(event: string, listener: (...args: any[]) => void): this { this._ee.on(event, listener); return this; }
    once(event: string, listener: (...args: any[]) => void): this { this._ee.once(event, listener); return this; }
    off(event: string, listener?: (...args: any[]) => void): this { this._ee.off(event, listener); return this; }
    emit(event: string, ...args: any[]): boolean { return this._ee.emit(event, ...args); }
}

export class Worker
{
    private _h: any;
    private _ee: EventEmitter;

    constructor(entry: string, options?: { workerData?: any })
    {
        this._ee = new EventEmitter();
        // Node 要求入口是绝对路径或以 './'/'../' 开头; 引擎按**运行根**解析裸相对串。
        // 这里把 './' 前缀剥掉, 于是 './helper/x.js' 与 'helper/x.js' 两种写法都能用。
        let e = String(entry);
        if (e.indexOf('./') === 0)
            e = e.slice(2);
        this._h = new NativeWorker(e, options || {});
        _workers.set(this._h.id, this);
        ensureDispatch();
        const self = this;
        this._h.on('exit', function (code: number): void
        {
            _workers.delete(self._h.id);
            self.emit('exit', code);
        });
    }

    /** 该 worker 的 id(主线程恒为 0, worker 非 0 —— 与 Node 的 threadId 一致) */
    get threadId(): number { return this._h.id; }
    get exited(): boolean { return this._h.exited; }
    get exitCode(): number { return this._h.exitCode; }

    postMessage(value: any): void { this._h.postMessage(value); }
    /** 同步结束该 worker(Node 的 terminate() 返回 Promise; 这里保持引擎的同步语义) */
    terminate(): void { _workers.delete(this._h.id); this._h.terminate(); }

    on(event: string, listener: (...args: any[]) => void): this { this._ee.on(event, listener); return this; }
    once(event: string, listener: (...args: any[]) => void): this { this._ee.once(event, listener); return this; }
    off(event: string, listener?: (...args: any[]) => void): this { this._ee.off(event, listener); return this; }
    emit(event: string, ...args: any[]): boolean { return this._ee.emit(event, ...args); }
}

export const isMainThread: boolean = _main;
export const threadId: number = __jz.worker.selfId();
/** 主线程为 null(同 Node); worker 侧是 new Worker 时随包进来的那份数据 */
export const workerData: any = _main
    ? null
    : (__jz.worker.workerData === undefined ? null : __jz.worker.workerData);

/** worker 侧与"创建它的那个引擎"通信的端口; 主线程为 null */
export const parentPort: MessagePort | null = (function (): MessagePort | null
{
    if (_main)
        return null;
    _port = new MessagePort(__jz.worker.parentId());
    ensureDispatch();
    return _port;
})();

/** 占位: 引擎每个 worker 都是全新空 global, 没有共享 env 这回事 */
export const SHARE_ENV: symbol = Symbol('worker_threads.SHARE_ENV');

const worker_threads = {
    Worker, MessagePort, parentPort, isMainThread, threadId, workerData, SHARE_ENV
};
export default worker_threads;
