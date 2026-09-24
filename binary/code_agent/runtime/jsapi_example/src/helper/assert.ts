// helper/assert.ts —— 例子共用的自检小工具(不是例子, ctest 不会单独跑它)。
//
// 约定的自检方式: 断言不成立就 throw。
//   * 同步: 模块顶层抛出 -> code_runtime 的 init 返回 false -> 退出码 1;
//   * 异步: 丢在未处理的 promise 上 -> 引擎上报 -> 退出码 1。
// 于是"例子还能不能跑"永远甩给 ctest 盯着, 不会随时间烂掉。

/** 打印一条结果; ok 为假时抛异常 */
export function check(name: string, ok: boolean): void {
    console.log((ok ? '[PASS] ' : '[FAIL] ') + name);
    if (!ok)
        throw new Error('check failed: ' + name);
}

/** 值比较(用 JSON 串比, 够例子用); 不等时打出两边的值 */
export function eq(name: string, got: unknown, want: unknown): void {
    const g = JSON.stringify(got);
    const w = JSON.stringify(want);
    if (g === w) {
        console.log('[PASS] ' + name + ' = ' + g);
        return;
    }
    console.log('[FAIL] ' + name + ' got=' + g + ' want=' + w);
    throw new Error('check failed: ' + name);
}

/** 把 setTimeout 包成可以 await 的延时(例子里到处要用, 放这里省得每份都抄一遍) */
export function delay(ms: number): Promise<void> {
    return new Promise<void>(function (resolve) {
        setTimeout(function () { resolve(); }, ms);
    });
}

/**
 * 吊一个空周期定时器把进程留住(见 09 的用法)。
 *
 * 宿主判断"还有没有活"看的是: **ref 着的定时器 / promise 微任务 / 登记过的原生异步(fetch) /
 * 打开着的套接字与服务端**(后两条是 2026-09-24 按 Node 的 handle ref 语义对齐后的行为)。
 * 所以等网络回包已经不需要它了; 还需要它的场景是"等待对象不被计入"的那些 ——
 * 例如等 worker 回信/退出(worker 线程不在句柄表里)。
 * 返回 Timeout: 不想被它吊住时可以 `.unref()`。
 * ⚠ 别漏了 clear: 定时器不清, 进程可能永远不退出。
 */
export function keepAlive(ms: number): Timeout {
    return setInterval(function () { }, ms);
}
