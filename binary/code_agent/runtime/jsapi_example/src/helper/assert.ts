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
 * "等外部事件"期间把进程吊住(见 06/07/09 的用法)。
 *
 * 为什么需要: 宿主(code_runtime)判断"还有没有活"看的是**引擎自己知道的那点活** ——
 * promise 微任务 / setTimeout 定时器 / 登记过的原生异步(fetch)。
 * 而等 TCP 回包、等 worker 回信这类阶段三者都为 0, 宿主会认为脚本已经跑完直接退出
 * (断言一条没跑也算"通过")。所以吊一个空周期定时器, 最后一步再 clear 掉。
 * ⚠ 别漏了 clear: 周期任务不清, 进程永远不退出。
 */
export function keepAlive(ms: number): number {
    return setInterval(function () { }, ms);
}
