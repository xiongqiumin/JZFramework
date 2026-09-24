// 09_worker —— 把 CPU 密集的活丢给另一个线程(worker), 主线程只管收发消息。
//
// 编译跑起来:  <tsc> -p tsconfig.json
//              code_runtime --entry <本目录>/build/09_worker.js
//
// 用的是 **Node 同名**的 `worker_threads`(引擎内置包, 包在私有的 __jz.worker 原子 API 之上):
//   import { Worker, isMainThread, threadId } from 'worker_threads';        // 主线程侧
//   import { parentPort, workerData, threadId } from 'worker_threads';      // worker 侧
// 所以这份写法在 Node 里也是对的(只有"入口路径按运行根解析"这一条是引擎语义)。
//
// worker 的关键事实:
//   * 每个 worker 是**独立引擎**: 全新环境, 不继承主线程的全局变量, 有自己的事件循环;
//   * 两边只靠消息通信(postMessage), 消息里的对象可以带共享引用与环;
//   * 入口脚本的路径按**运行根**解析 = 入口 js 所在的 build/ 目录, 所以这里写
//     'helper/worker_sum.js'(不带 './', 也不带 src/);
//   * 用完 w.terminate() 收掉, 不然线程一直挂着。
//
// 等 worker 回信那段**不在**引擎的句柄表里(worker 线程不计"算活"), 所以照旧要吊一个
// 定时器保活; 与网络用例(06/07, 套接字自己撑住)的区别就在这里。

import { Worker, isMainThread, threadId } from 'worker_threads';
import { check, keepAlive } from './helper/assert.js';

const keep = keepAlive(50);

console.log('--- 09_worker ---');

check('主线程 threadId 恒为 0', threadId === 0);
check('主线程 isMainThread = true', isMainThread === true);

// './' 前缀: Node 强制要求(绝对 / './' / '../'); 引擎侧会把它剥掉再按运行根解析
const w = new Worker('./helper/worker_sum.js', { workerData: { tag: 'sum-1' } });
console.log('spawned worker id=' + w.threadId);
check('worker 的 threadId 不是主线程', w.threadId !== 0);

let answered = false;

// 主线程收消息: Node 形态 —— 回调只给消息本身(发送方身份让 worker 自己带回)
w.on('message', function (msg: any): void {
    if (answered)
        return;
    answered = true;

    check('消息来自那个 worker', msg.workerId === w.threadId);
    check('worker 侧 isMainThread = false', msg.isMain === false);
    check('worker 拿到了 workerData', msg.tag === 'sum-1');
    check('1..100 的区间和 = 5050', msg.sum === 5050);

    w.terminate();
    clearInterval(keep);
    console.log('09_worker done');
});

w.postMessage({ op: 'sum', from: 1, to: 100 });
