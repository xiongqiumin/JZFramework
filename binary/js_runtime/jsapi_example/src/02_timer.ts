// 02_timer —— 定时器三件套: setTimeout / setInterval / clearTimeout / clearInterval。
//
// 编译跑起来:  <tsc> -p tsconfig.json
//              code_runtime --entry <本目录>/build/02_timer.js
//
// 语义与浏览器一致: 都是"一次性"或"周期任务", 返回 id 用于取消。
// ⚠ 别用 sleep(ms)(全局那个会阻塞整个线程, 定时器与网络回调都跟着停);
//   想等一会儿就 await delay(ms)(见 helper/assert.ts)。

import { check, delay } from './helper/assert.js';

console.log('--- 02_timer ---');

// 1) 周期任务: 到点了停掉, 停掉之后不能再触发
let ticks = 0;
const id = setInterval(function () {
    ticks = ticks + 1;
}, 20);

// 2) 一次性任务: 取消之后不该再触发(若触发会 throw -> 未处理拒绝 -> 例子失败)
const cancelId = setTimeout(function () {
    throw new Error('这个定时器应该已经被 clearTimeout 取消掉了');
}, 30);
clearTimeout(cancelId);

setTimeout(async function () {
    clearInterval(id);
    const stopped = ticks;
    await delay(60);
    check('clearInterval 之后不再触发', ticks === stopped);
    check('interval 期间至少触发了 2 次', stopped >= 2);

    // 3) setTimeout(fn, 0) 允许: 下一轮事件泵尽快跑
    let zeroFired = false;
    setTimeout(function () { zeroFired = true; }, 0);
    await delay(20);
    check('setTimeout(fn, 0) 会被执行', zeroFired);

    console.log('02_timer done (ticks=' + stopped + ')');
}, 120);
