// 05_url_events —— URL 解析与事件总线(设备/服务端脚本里最常见的两件小事)。
//
// 编译跑起来:  <tsc> -p tsconfig.json
//              code_runtime --entry <本目录>/build/05_url_events.js
//
// 两个来源要分清:
//   * URL / URLSearchParams / WebSocket / EventEmitter 是**运行期全局**(引擎装的, 不用 import);
//   * 'url' / 'events' 这两个包名也有导出, 想写清晰一点就显式 import —— 拿到的是同一份实现。
//   * ⚠ EventEmitter 与 Node 一致: 没有 'error' 监听器时 `emit('error', e)` 会**抛异常**(不会静默吞掉)。
//     所以 socket / worker 等出错了想拦住, 必须自己 `on('error', ...)`; 否则异常会冒泡成脚本失败。

import { EventEmitter } from 'events';
import { check } from './helper/assert.js';

console.log('--- 05_url_events ---');

// ---- 1) URL: 组装 query 比自己拼字符串稳 ----
const url = new URL('http://127.0.0.1:8080/api/devices?page=2&size=20');
check('protocol', url.protocol === 'http:');
check('hostname', url.hostname === '127.0.0.1');
check('port', url.port === '8080');
check('pathname', url.pathname === '/api/devices');
check('search', url.search === '?page=2&size=20');

const sp = url.searchParams;
check('searchParams.get', sp.get('page') === '2');
sp.set('page', '3');
sp.append('kw', 'pump');
console.log('searchParams -> ' + sp.toString());

// 反过来: 从零拼一个查询串(上报接口常用)
const q = new URLSearchParams({ device: 'pump-01', value: '36.5' });
check('URLSearchParams 组装', q.toString() === 'device=pump-01&value=36.5');

// ---- 2) EventEmitter: 模块之间解耦用(采集 -> 入库/上报各自监听) ----
const bus = new EventEmitter();
const seen: string[] = [];

function onSample(v: number): void { seen.push('sample:' + v); }
bus.on('sample', onSample);
bus.once('sample', function (v: number) { seen.push('once:' + v); });

bus.emit('sample', 36.5);       // on + once      -> sample, once
bus.emit('sample', 37.1);       // once 已消耗 -> 只剩 on
bus.off('sample', onSample);
bus.emit('sample', 38.0);       // 已摘除 -> 没人再收到

check('on / once / off 的触发顺序', seen.join(',') === 'sample:36.5,once:36.5,sample:37.1');

// 不带 payload / 多参数的 emit 都行
let sum = 0;
const bus2 = new EventEmitter();
bus2.on('add', function (a: number, b: number) { sum = a + b; });
bus2.emit('add', 2, 3);
check('emit 带多个参数', sum === 5);

console.log('05_url_events done');
