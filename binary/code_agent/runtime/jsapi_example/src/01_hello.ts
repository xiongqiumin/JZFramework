// 01_hello —— 最小的一份 JZ TS 脚本: 不用任何内置模块, 只用引擎装好的那些全局。
//
// 编译跑起来:  <tsc> -p tsconfig.json                       (本目录 = jsapi_example)
//              code_runtime --entry <本目录>/build/01_hello.js
//              (--entry 给的是哪份 js, 就以它**所在的目录**为运行根/工作目录)
//
// 这里能看到的运行期约定:
//   * 只认 ESM(import / export)—— 没有 require, 也没有 module.exports;
//   * console.log / setTimeout / setInterval / Promise 都是引擎直接装在 globalThis 上的;
//   * 相对 import 按**脚本所在目录**解析 —— TS 侧写 './helper/assert.js'(带 .js),
//     编译后说明符原样保留, 落到 build/helper/assert.js, 运行期直接可用。
//     类型来自仓库里的 js_wrapper 声明(见 tsconfig.json 的 include)。

import { check, delay } from './helper/assert.js';

console.log('--- 01_hello ---');

// 1) 语言本身: 标准 quickjs, 语法没什么特别的
check('算术 1 + 2 * 3 === 7', 1 + 2 * 3 === 7);
check('字符串拼接', 'hello ' + 'world' === 'hello world');
check('数组 map/join', [1, 2, 3].map(function (x: number) { return x * 2; }).join(',') === '2,4,6');
check('JSON.stringify', JSON.stringify({ a: 1, b: [2, 3] }) === '{"a":1,"b":[2,3]}');

// 2) 异步: 写 async 函数 + await 就行。
//    注意: 模块顶层的同步部分跑完进程不会立刻退 —— 还挂着 setTimeout / 打开着的套接字都算
//    "有事要等"(Node 的 handle ref 语义), 宿主会继续泵到它们收摊为止; 见 helper/assert.ts。
(async function main(): Promise<void> {
    const t0 = Date.now();
    await delay(50);
    const used = Date.now() - t0;
    check('await 一个 setTimeout(50) 至少要等 45ms', used >= 45);

    const now = new Date();
    check('Date 可用', now.getTime() > 0);
    console.log('01_hello done (waited ' + used + 'ms)');
})();
