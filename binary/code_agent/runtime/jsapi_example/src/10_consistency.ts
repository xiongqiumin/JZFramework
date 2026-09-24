// 10_consistency —— 锁住"与 Node 一致"的那几条约定(行为不能有反例, 否则 ctest 红)。
//
// 编译跑起来:  <tsc> -p tsconfig.json
//              code_runtime --entry <本目录>/build/10_consistency.js
//
// 这份不是教新 API, 而是给前面几份例子的"一致性"兜底:
//   * EventEmitter 没有 'error' 监听器时 emit('error') 必须**抛异常**(Node 语义, 错误不静默吞);
//   * http 服务端 req.headers 是**普通对象**(按下标 key 取, 不是 Web Headers 实例);
//   * path.sep 全平台都是 '/'(与引擎内部路径归一化、join 输出一致)。
// 这三条任何一条回退, 这份例子都会失败并让 ctest 报红。

import http from 'http';
import type { IncomingMessage, ServerResponse } from 'http';
import { EventEmitter } from 'events';
import path from 'path';
import { check } from './helper/assert.js';

console.log('--- 10_consistency ---');

// 1) path.sep 与引擎一致: 全平台都是 '/'(引擎把路径都归一化成 '/')
check("path.sep === '/'", path.sep === '/');

// 2) EventEmitter: 没有 'error' 监听器时 emit('error') 抛异常(Node 语义)
{
    const e = new EventEmitter();
    let threw = false;
    try {
        e.emit('error', new Error('boom'));   // 无监听器 -> 必须抛
    } catch (err) {
        threw = (err instanceof Error) && err.message === 'boom';
    }
    check('无 error 监听器: emit(error) 抛异常', threw);

    // 非 Error 参数也抛, 且消息对齐 Node v26: Unhandled error. ('<arg>')
    let threw2 = false, msg2 = '';
    try {
        e.emit('error', 'just a string');
    } catch (err) {
        if (err instanceof Error) { threw2 = true; msg2 = err.message; }
    }
    check("无 error 监听器: 非 Error 参数也抛(消息=Node 'Unhandled error. (...)')",
          threw2 && msg2 === "Unhandled error. ('just a string')");

    // 有监听器时正常交付, 不抛
    let got: unknown = null;
    const e2 = new EventEmitter();
    e2.on('error', function (x: Error) { got = x; });
    e2.emit('error', new Error('handled'));
    check('有 error 监听器: 不抛, 正常交付', got instanceof Error && (got as Error).message === 'handled');
}

// 3) http 服务端 req.headers 是普通对象(可按字符串 key 取, 不是 Web Headers 实例)
const PORT = 23410;
const base = 'http://127.0.0.1:' + PORT;
const server = http.createServer(function (req: IncomingMessage, res: ServerResponse) {
    check('req.headers 不是 Headers 实例', !(req.headers instanceof (globalThis as any).Headers));
    check("req.headers['content-type'] 可直接按下标取", req.headers['content-type'] === 'text/plain');
    res.writeHead(200, { 'Content-Type': 'text/plain' });
    res.end('ok');
});
server.listen(PORT, '127.0.0.1', async function () {
    const r = await fetch(base, { method: 'POST', headers: { 'Content-Type': 'text/plain' }, body: 'x' });
    await r.text();
    server.close();     // 监听句柄收掉, 进程才会退出(句柄默认吊住进程, 见 06/07)
    console.log('10_consistency done');
});
