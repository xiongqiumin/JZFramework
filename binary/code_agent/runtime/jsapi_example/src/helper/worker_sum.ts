// helper/worker_sum.ts —— 09_worker 例子的 worker 入口(worker 侧脚本不是例子本身)。
//
// worker 里是**另一个引擎**: 不继承主线程的全局变量; 用 Node 同名的 worker_threads 里的
// parentPort 跟父引擎通信(主线程侧那个 parentPort 是 null)。
// 入口脚本的路径按**运行根**(= 入口 js 所在目录, 即 build/)解析,
// 所以主线程那边写 'helper/worker_sum.js'。

import { parentPort, workerData, isMainThread, threadId } from 'worker_threads';

interface SumTask {
    op: string;
    from: number;
    to: number;
}

const port = parentPort!;       // worker 侧一定存在(只有主线程才是 null)

// 收到消息 -> 算区间和 -> 用 parentPort 投回去(不用知道父引擎的 id)
port.on('message', function (task: SumTask): void {
    if (task.op !== 'sum') {
        port.postMessage({ op: 'error', reason: 'unknown op: ' + task.op });
        return;
    }

    let s = 0;
    for (let i = task.from; i <= task.to; i++)
        s += i;

    // 创建时随包进来的那份数据(主线程 new Worker 时给的 workerData)
    const data = workerData as { tag: string } | null;

    port.postMessage({
        op: 'sum',
        from: task.from,
        to: task.to,
        sum: s,
        tag: data ? data.tag : null,
        // worker 自己的身份: threadId != 0, isMainThread = false
        workerId: threadId,
        isMain: isMainThread,
    });
});
