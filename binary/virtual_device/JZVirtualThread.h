#ifndef JZ_VIRTUAL_THREAD_H_
#define JZ_VIRTUAL_THREAD_H_
#include "JZThread.h"
#include "JZSemaphore.h"
#include <functional>
#include <memory>

// ==================== 跨线程同步调用 (虚拟桌面/进程内部用) ====================
// 在 thread 上跑一段代码并等它跑完 —— 桌面 <-> 进程之间 RPC 的全部实现就这一句
// (底层是框架自带的 runOnThread: 往目标线程的事件队列投一个 lambda)。
//
// 返回 false 只有两种可能: 线程没在跑, 或等了 timeoutMs 还没轮上。
// ⚠ 两条纪律(跨线程调用的通例, 也是这里唯一的坑):
//   1) func 里**不要**再回等调用方线程 (会死锁);
//   2) 超时后那个 lambda 仍可能在之后执行 ⇒ func 捕获的东西必须活得够久
//      (捕 shared_ptr/值, 别捕调用方的栈上引用)。
inline bool jzRunOnThreadSync(JZThread* thread, const std::function<void()>& func,
                              int timeoutMs = 0)
{
    if (!thread)
        return false;
    if (JZThread::currentThread() == thread) {
        func();                       // 已经在本线程: 直接跑
        return true;
    }
    if (!thread->isRunning())
        return false;

    auto done = std::make_shared<JZSemaphore>();
    auto fn = func;                   // 拷贝一份 (调用方的栈可能比 lambda 先没)
    runOnThread(thread, [fn, done]() { fn(); done->release(); });
    if (timeoutMs <= 0) {
        done->acquire();
        return true;
    }
    return done->tryAcquire(1, timeoutMs);
}

// 异步收尾 (回调版队列屏障): **投一个回调**过去 —— 它被跑到就说明"在这之前投给这个线程的
// 东西都处理完了" (队列 FIFO)。**调用方不等**。
//   · 要"等"的东西 (点完就截图、响应里报告"已处理") 一律挂在这个回调上, 不要在调用方阻塞:
//     "投过去再阻塞等"就是互等配方 (目标线程可能正回等我们这边, 实测踩过几次);
//   · done 在**目标线程**上跑。要回到调用方线程就在 done 里再投一次 (runOnObject/runOnThread)。
inline void jzAfterPending(JZThread* thread, const std::function<void()>& done)
{
    if (!thread || !done)
        return;
    if (JZThread::currentThread() == thread) {
        done();                       // 本线程: 没有"投出去还没跑"的东西
        return;
    }
    if (!thread->isRunning())
        return;
    runOnThread(thread, done);
}

#endif // JZ_VIRTUAL_THREAD_H_
