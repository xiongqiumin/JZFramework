// helper/worker_sum.ts —— 09_worker 例子的 worker 入口(worker 侧脚本不是例子本身)。
//
// worker 里是**另一个引擎**: 不继承主线程的全局变量, 只认 __jz.worker.* 这套 API
// (类型声明见 src/globals.d.ts)。
// 入口脚本的路径按**运行根**(= 入口 js 所在目录, 即本目录的 build/)解析,
// 所以主线程那边写 'helper/worker_sum.js'。

interface SumTask {
    op: string;
    from: number;
    to: number;
}

// 收到消息 -> 算区间和 -> 把结果投回发送方(from 就是发送方的 id)
__jz.worker.onMessage(function (from: number, task: SumTask): void {
    if (task.op !== 'sum') {
        __jz.worker.postTo(from, { op: 'error', reason: 'unknown op: ' + task.op });
        return;
    }

    let s = 0;
    for (let i = task.from; i <= task.to; i++)
        s += i;

    // 创建时随包进来的那份数据(主线程 new Worker 时给的 workerData)
    const data = __jz.worker.workerData as { tag: string } | null;

    __jz.worker.postTo(from, {
        op: 'sum',
        from: task.from,
        to: task.to,
        sum: s,
        tag: data ? data.tag : null,
        // worker 自己的身份: id != 0, isMainThread = false, parentId = 创建它的那个 id
        workerId: __jz.worker.selfId(),
        isMain: __jz.worker.isMainThread(),
        parent: __jz.worker.parentId(),
    });
});
