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
 * 吊一个空周期定时器把进程留住。
 *
 * 宿主判断"还有没有活"看的是**句柄与待办**(2026-09-24 起完全对齐 Node 的 handle ref 语义):
 * ref 着的定时器 / 打开着的套接字与服务端 / 跑着的 worker / promise 微任务 / **在途的异步请求**
 * (fetch / modbus 读写 —— Node 里就是"请求用着的那条 socket 是 ref 的", 实测只发一个 fetch 不 await
 * 也会等到回包才退)。所以现在**等网络回包、等 worker 回信都不再需要它** —— 那些句柄自己就把进程
 * 撑住了(反过来说: 用完得 close()/terminate(), 否则进程不退出)。
 * 这个工具留给"等的东西不被计入"的场景, 或者需要显式表达"我要多活一会儿"时用。
 * 返回 Timeout: 不想被它吊住时可以 `.unref()`。
 * ⚠ 别漏了 clear: 定时器不清, 进程可能永远不退出。
 */
export function keepAlive(ms: number): Timeout {
    return setInterval(function () { }, ms);
}
