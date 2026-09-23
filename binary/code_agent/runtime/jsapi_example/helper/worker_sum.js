// helper/worker_sum.js —— 09_worker 例子的 worker 入口(worker 侧脚本不是例子本身)。
//
// worker 里是**另一个引擎**: 不继承主线程的全局变量, 只认 __jz.worker.* 这套 API。
// 入口脚本的路径按运行根(例子目录)解析, 所以主线程那边写 'helper/worker_sum.js'。

// 收到消息 -> 算区间和 -> 把结果投回发送方(from 就是发送方的 id)
__jz.worker.onMessage(function (from, task) {
    if (task.op !== 'sum') {
        __jz.worker.postTo(from, { op: 'error', reason: 'unknown op: ' + task.op });
        return;
    }

    let s = 0;
    for (let i = task.from; i <= task.to; i++)
        s += i;

    __jz.worker.postTo(from, {
        op: 'sum',
        from: task.from,
        to: task.to,
        sum: s,
        tag: __jz.worker.workerData ? __jz.worker.workerData.tag : null,   // 创建时随包进来的那份数据
        // worker 自己的身份: id != 0, isMainThread = false, parentId = 创建它的那个 id
        workerId: __jz.worker.selfId(),
        isMain: __jz.worker.isMainThread(),
        parent: __jz.worker.parentId(),
    });
});
