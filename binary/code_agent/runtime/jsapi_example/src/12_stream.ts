// 12_stream —— Node stream 兼容子集(Readable/Writable/Transform/PassThrough/pipeline)。
//
// 这些通过 `import { ... } from 'stream'` 使用(和 Node 一样是模块, 不是全局)。

import { check, eq } from './helper/assert.js';
import { Readable, Writable, Transform, PassThrough, pipeline, Duplex } from 'stream';

console.log('--- 12_stream ---');

// 基本导出存在
check('stream 导出', typeof Readable === 'function' && typeof Writable === 'function'
    && typeof Transform === 'function' && typeof PassThrough === 'function'
    && typeof pipeline === 'function' && typeof Duplex === 'function');

// Readable.from + pipe -> Writable(拼接)
{
    let out = '';
    const r = Readable.from(['hello', ' ', 'world']);
    const w = new Writable({ write(chunk: any, _enc: any, cb: any) { out += String(chunk); cb(); } });
    r.pipe(w);
    w.on('finish', () => { eq('stream: pipe 拼接', out, 'hello world'); });
}

// Transform(转大写) 串接
{
    let out = '';
    const r = Readable.from(['ab', 'cd']);
    const t = new Transform({ transform(chunk: any, _enc: any, cb: any) { cb(null, String(chunk).toUpperCase()); } });
    const w = new Writable({ write(chunk: any, _enc: any, cb: any) { out += String(chunk); cb(); } });
    r.pipe(t).pipe(w);
    w.on('finish', () => { eq('stream: transform 转大写', out, 'ABCD'); });
}

// pipeline + PassThrough
{
    let out = '';
    const r = Readable.from(['x', 'y']);
    const p = new PassThrough();
    const w = new Writable({ write(chunk: any, _enc: any, cb: any) { out += String(chunk); cb(); } });
    pipeline(r, p, w, (err: any) =>
    {
        check('stream: pipeline 无错误', !err);
        eq('stream: pipeline 拼接', out, 'xy');
    });
}

// Duplex 两端都可用(这里当 PassThrough 用)
// ⚠ Node 里 Duplex 必须自己给出两端实现(options 的 read / write), 裸 new Duplex() 会抛
//   ERR_METHOD_NOT_IMPLEMENTED; 引擎里裸构造可以(默认透传), 但照 Node 的写法两边都能跑。
{
    let out = '';
    const r = Readable.from(['z']);
    const d = new Duplex({
        read(): void { },                                       // 可读侧: 这里不主动产数据
        write(this: any, chunk: any, _enc: any, cb: any): void { this.push(chunk); cb(); },
        final(this: any, cb: any): void { this.push(null); cb(); },   // 收尾: 关掉可读侧, 下游才 end
    });
    const w = new Writable({ write(chunk: any, _enc: any, cb: any) { out += String(chunk); cb(); } });
    r.pipe(d).pipe(w);
    w.on('finish', () => { eq('stream: duplex 透传', out, 'z'); });
}
