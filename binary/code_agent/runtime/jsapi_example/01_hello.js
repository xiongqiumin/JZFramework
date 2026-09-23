// 01_hello —— 最小的一份 JZ JS 脚本: 不用任何内置模块, 只用引擎装好的那些全局。
//
// 跑起来:  code_runtime --entry <本文件>
//          (--entry 给的是哪份 js, 就以它**所在的目录**为运行根/工作目录)
//
// 这里能看到的运行期约定:
//   * 只认 ESM(import / export)—— 没有 require, 也没有 module.exports;
//   * console.log / setTimeout / setInterval / Promise 都是引擎直接装在 globalThis 上的;
//   * 相对 import('./helper/assert.js')按**脚本所在目录**解析, 直接可用。

import { check, delay } from './helper/assert.js';

console.log('--- 01_hello ---');

// 1) 语言本身: 标准 quickjs, 语法没什么特别的
check('算术 1 + 2 * 3 === 7', 1 + 2 * 3 === 7);
check('字符串拼接', 'hello ' + 'world' === 'hello world');
check('数组 map/join', [1, 2, 3].map(function (x) { return x * 2; }).join(',') === '2,4,6');
check('JSON.stringify', JSON.stringify({ a: 1, b: [2, 3] }) === '{"a":1,"b":[2,3]}');

// 2) 异步: 写 async 函数 + await 就行。
//    注意: 模块顶层的同步部分跑完进程不会立刻退 —— 还挂着 setTimeout 就算"有待办任务",
//    宿主会继续泵到它结算完为止(这是 await 延时能跑完的原因; 但等网络回包不算, 见 helper/assert.js)。
(async function main() {
    const t0 = Date.now();
    await delay(50);
    const used = Date.now() - t0;
    check('await 一个 setTimeout(50) 至少要等 45ms', used >= 45);

    const now = new Date();
    check('Date 可用', now.getTime() > 0);
    console.log('01_hello done (waited ' + used + 'ms)');
})();
