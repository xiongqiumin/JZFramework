/// <reference path="../events/types/ambient.d.ts" />
/**
 * @jz/stream —— Node stream 兼容子集(零依赖)。
 *
 * 范围(够覆盖绝大多数入门/agent 代码): Readable / Writable / Duplex / Transform / PassThrough /
 * pipeline / finished。语义对齐 Node 的"事件 + pipe"模型:
 *   - 源(Readable/Transform/Duplex) `emit('data', chunk)`, 监听 'data' 进入流动模式;
 *   - `pipe(dest)` 把 'data' 接到 dest.write, 源 'end' 时 dest.end;
 *   - Transform 在 _transform(chunk, cb) 里把处理后结果 cb(null, out), 结果从可读侧 emit('data')。
 *
 * 已知简化(与 Node 的差异, 不影响常见用法):
 *   - 不做真正的背压: write 永远返回 true、立即交付, 不做 highWaterMark 节流;
 *   - 同步/微任务交付, 没有 Node 的异步延迟排队;
 *   - 游标式顺序读写请用已有的 `bytes`(ByteReader/ByteWriter), 这里只做"数据流经管道"。
 *
 * stream 是**模块**(Node 里也是 require('stream'), 不是全局); 不用 import 时也可
 * `import { Readable, Writable, Transform, PassThrough, pipeline } from 'stream'`。
 *
 * 实现说明: Stream 内部组合一个 EventEmitter(而非 extends), 规避部分 TS 环境下
 * `extends` 内置事件类的成员解析问题; 运行时行为一致。
 */

'use strict';

import { EventEmitter } from 'events';

type Chunk = any;
type Callback = (err?: any) => void;

export class Stream
{
    private _ee: EventEmitter;
    constructor()
    {
        this._ee = new EventEmitter();
    }
    on(event: string, listener: (...args: any[]) => void): this { this._ee.on(event, listener); return this; }
    once(event: string, listener: (...args: any[]) => void): this { this._ee.once(event, listener); return this; }
    off(event: string, listener?: (...args: any[]) => void): this { this._ee.off(event, listener); return this; }
    emit(event: string, ...args: any[]): boolean { return this._ee.emit(event, ...args); }
    removeListener(event: string, listener?: (...args: any[]) => void): this { this._ee.removeListener(event, listener); return this; }
    listenerCount(event: string): number { return this._ee.listenerCount(event); }
    eventNames(): string[] { return this._ee.eventNames(); }
}

export class Readable extends Stream
{
    private _bufs: Chunk[] = [];
    private _ended = false;
    private _flowing = false;

    constructor(opts?: any)
    {
        super();
        if (opts && typeof opts.read === 'function') this._read = opts.read;
    }

    _read(size?: number): void { void size; }

    /** 塞入一块数据; 传 null 表示结束 */
    push(chunk: Chunk): boolean
    {
        if (chunk === null || chunk === undefined) { this._end(); return false; }
        this._bufs.push(chunk);
        if (this._flowing) this.emit('data', chunk);
        return true;
    }

    private _end(): void
    {
        if (this._ended) return;
        this._ended = true;
        this.emit('end');
    }

    on(event: string, listener: (...args: any[]) => void): this
    {
        super.on(event, listener);
        if (event === 'data')
        {
            this._flowing = true;
            for (const c of this._bufs) this.emit('data', c);
            this._bufs.length = 0;
        }
        return this;
    }

    pipe<T extends Writable>(dest: T): T
    {
        this.on('data', (c: Chunk) => (dest as any).write(c));
        this.on('end', () => (dest as any).end());
        return dest;
    }

    /** 从一个(异步)可迭代对象造 Readable */
    static from(iterable: Iterable<Chunk> | AsyncIterable<Chunk>): Readable
    {
        const r = new Readable();
        (async () =>
        {
            for await (const x of iterable as any) r.push(x);
            r.push(null);
        })();
        return r;
    }
}

export class Writable extends Stream
{
    constructor(opts?: any)
    {
        super();
        // Node 形态: new Writable({ write(chunk, enc, cb) { ... } }) —— 不给就走默认(丢弃)
        if (opts && typeof opts.write === 'function') this._write = opts.write;
        if (opts && typeof opts.final === 'function') this._final = opts.final;
    }

    _write(chunk: Chunk, enc: string, cb: Callback): void { void enc; cb(); }
    _final(cb: Callback): void { cb(); }

    write(chunk: Chunk, enc?: string | Callback, cb?: Callback): boolean
    {
        const callback: Callback | undefined = typeof enc === 'function' ? enc : cb;
        const encStr: string = typeof enc === 'string' ? enc : '';
        this._write(chunk, encStr, () => { if (callback) callback(); });
        this.emit('data', chunk);
        return true;
    }

    end(chunk?: Chunk, cb?: Callback): this
    {
        if (chunk !== undefined && chunk !== null) this.write(chunk);
        this._final(() => { this.emit('finish'); if (cb) cb(); });
        return this;
    }
}

export class Transform extends Readable
{
    constructor(opts?: any)
    {
        super(opts);
        if (opts && typeof opts.transform === 'function') this._transform = opts.transform;
        if (opts && typeof opts.final === 'function') this._final = opts.final;
    }

    _transform(chunk: Chunk, enc: string, cb: (err: any, out?: Chunk) => void): void { void enc; cb(null, chunk); }
    _final(cb: Callback): void { cb(); }

    write(chunk: Chunk, enc?: string | Callback, cb?: Callback): boolean
    {
        const callback: Callback | undefined = typeof enc === 'function' ? enc : cb;
        this._transform(chunk, typeof enc === 'string' ? enc : '', (err, out) =>
        {
            if (out !== undefined && out !== null) this.emit('data', out);
            if (callback) callback(err);
        });
        return true;
    }

    end(chunk?: Chunk, cb?: Callback): this
    {
        if (chunk !== undefined && chunk !== null) this.write(chunk);
        // 可读侧的结束走 push(null)(带 _ended 幂等), 别直接 emit('end') ——
        // 否则 final 里已经 push(null) 时会重复发一次, 下游 end() 被调两遍。
        this._final(() => { this.emit('finish'); this.push(null); if (cb) cb(); });
        return this;
    }
}

export class Duplex extends Transform
{
    private _customWrite = false;

    constructor(opts?: any)
    {
        super(opts);
        // Node 的 Duplex 两头都得自己给(options 的 read / write)。给了 write 就走 _write 路径
        // (同 Node: 由实现自己 push 到可读侧); 什么都不给才退回 Transform 的 _transform 透传。
        if (opts && typeof opts.write === 'function')
        {
            this._write = opts.write;
            this._customWrite = true;
        }
    }

    _write(chunk: Chunk, enc: string, cb: Callback): void { void enc; cb(); }

    write(chunk: Chunk, enc?: string | Callback, cb?: Callback): boolean
    {
        if (!this._customWrite)
            return super.write(chunk, enc, cb);
        const callback: Callback | undefined = typeof enc === 'function' ? enc : cb;
        const encStr: string = typeof enc === 'string' ? enc : '';
        this._write(chunk, encStr, () => { if (callback) callback(); });
        return true;
    }
}
export class PassThrough extends Transform
{
    _transform(chunk: Chunk, enc: string, cb: (err: any, out?: Chunk) => void): void { void enc; cb(null, chunk); }
}

/** 串起多个流, 最后一个 'finish'/'end' 或任何 'error' 时回调 cb(err) */
export function pipeline(...args: any[]): any
{
    const cb: Callback = args[args.length - 1];
    const streams: any[] = args.slice(0, -1);
    let prev = streams[0];
    for (let i = 1; i < streams.length; i++)
    {
        const cur = streams[i];
        prev.pipe(cur);
        prev = cur;
    }
    const last = streams[streams.length - 1];
    let done = false;
    const finish = (err?: any) =>
    {
        if (!done) { done = true; if (cb) cb(err); }
    };
    last.on('finish', () => finish());
    last.on('end', () => finish());
    streams.forEach((s) => s.on('error', (e: any) => finish(e)));
    return last;
}

/** 等一个流结束(finish/end)或出错, 回调 cb(err) */
export function finished(stream: any, cb: Callback): void
{
    let done = false;
    const f = (err?: any) =>
    {
        if (!done) { done = true; cb(err); }
    };
    stream.on('finish', () => f());
    stream.on('end', () => f());
    stream.on('error', (e: any) => f(e));
}

export default { Stream, Readable, Writable, Duplex, Transform, PassThrough, pipeline, finished };
