#ifndef JZCO_WATCH_H_
#define JZCO_WATCH_H_
#include <vector>
#include "JZObject.h"
#include "JZEventLoop.h"
#include "JZCoCoroutine.h"

/*
JZCoWatch: 基于 JZCoScheduler::sigFinished(JZCoCoroutine *co) 的协程监视器

根据调用上下文自动选择等待方式:
  - 协程内:    用 JZCoCondition 让出(调度出去), 由 sigFinished 唤醒, 不阻塞主线程调度
  - 主线程(非协程): 用 JZEventLoop 驱动事件循环等待
详见 JZCoCoroutine.h 顶部说明: 协程内不能跑事件循环(会阻塞主线程导致调度死锁)。
*/
class JZFRAMEWORK_API JZCoWatch : public JZObject
{
public:
    explicit JZCoWatch(JZObject *parent = nullptr);
    ~JZCoWatch();

    // 等待列表中的任意一个协程执行完毕(先到先得), 返回结束的协程指针
    // 列表为空或已全部结束时立即返回 nullptr
    JZCoCoroutinePtr waitAny(const std::vector<JZCoCoroutinePtr> &co_list);

    // 等待列表中所有协程执行完毕
    void waitAll(const std::vector<JZCoCoroutinePtr> &co_list);

    // 等待调度器中所有协程执行完毕(不依赖某一时刻快照)
    // 每收到一个 sigFinished 就检查调度器里是否已无未结束协程,
    // 因此协程内部再启动的协程也会被一并等待。
    void waitAllCoroutines();

protected:
    void onFinished(JZCoCoroutine *co);

    // 根据上下文阻塞等待 / 唤醒等待者
    void blockWait();   // 协程内 m_cond.wait(); 非协程 m_loop.exec()
    void wakeWait();    // 协程内 m_cond.wakeOne(); 非协程 m_loop.quit()

    enum class Mode { None, Any, All, Scheduler };

    JZEventLoop m_loop;
    JZCoCondition m_cond;                     // 协程内等待条件
    std::vector<JZCoCoroutinePtr> m_watchList; // 仍被监听的协程(持有 shared_ptr 保持存活)
    Mode m_mode = Mode::None;
    JZCoCoroutine *m_anyFinished = nullptr;    // waitAny 的结果(原始指针)
};

#endif // !JZCO_WATCH_H_
