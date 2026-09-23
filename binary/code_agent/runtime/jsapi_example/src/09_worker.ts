// 09_worker —— 把 CPU 密集的活丢给另一个线程(worker), 主线程只管收发消息。
//
// 编译跑起来:  <tsc> -p tsconfig.json
//              code_runtime --entry <本目录>/build/09_worker.js
//
// worker 的关键事实:
//   * 每个 worker 是**独立引擎**: 全新环境, 不继承主线程的全局变量, 有自己的事件循环;
//   * 两边只靠消息通信(postMessage / __jz.worker.postTo), 消息里的对象可以带共享引用与环;
//   * 入口脚本的路径按**运行根**解析 = 入口 js 所在的 build/ 目录, 所以这里写
//     'helper/worker_sum.js'(不带 './', 也不带 src/);
//   * 用完 w.terminate() 收掉(或 worker 自己 __jz.worker.exit()), 不然线程一直挂着。
//
// 等 worker 回信的那段"没有待办任务"(理由见 06_net_echo), 所以同样吊一个定时器保活。
// worker 那套 API 的类型声明在 src/globals.d.ts(js_wrapper 里还没有这份契约)。

import { check, keepAlive } from './helper/assert.js';

const keep = keepAlive(50);

console.log('--- 09_worker ---');

check('主线程 id 恒为 0', __jz.worker.selfId() === 0);
check('isMainThread()', __jz.worker.isMainThread() === true);
check('主线程没有父 worker', __jz.worker.parentId() === -1);

// workerData: 创建时随包进去的一份数据(worker 里用 __jz.worker.workerData 取)
const w = new Worker('helper/worker_sum.js', { workerData: { tag: 'sum-1' } });
console.log('spawned worker id=' + w.id);

let answered = false;

// 主线程收消息: from 是发送方 id
__jz.worker.onMessage(function (from: number, msg: any): void {
    if (answered)
        return;
    answered = true;

    check('消息来自那个 worker', from === w.id);
    check('worker 侧 isMainThread = false', msg.isMain === false);
    check('worker 的 parentId = 主线程', msg.parent === 0);
    check('worker 拿到了 workerData', msg.tag === 'sum-1');
    check('1..100 的区间和 = 5050', msg.sum === 5050);

    w.terminate();
    clearInterval(keep);
    console.log('09_worker done');
});

w.postMessage({ op: 'sum', from: 1, to: 100 });
