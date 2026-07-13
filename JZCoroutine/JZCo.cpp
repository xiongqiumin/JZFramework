#include "JZCo.h"
#include "JZCoWatch.h"
#include "JZPlatform.h"
#include "JZTimer.h"
#include "JZApplication.h"
#include "JZDebug.h"

void jzco_init()
{
    auto co = new JZCoScheduler(jzApp);
    co->init();
}

void jzco_yield()
{
    auto co = g_scheduler->currentCoroutine();
    g_scheduler->yield(co);
}

void jzco_resume(JZCoCoroutine* co)
{
    g_scheduler->resume(co);
}

void jzco_sleep(int ms)
{
    JZCoCoroutine* task = g_scheduler->currentCoroutine();
    JZTimer t;
    t.setSingleShot(true);
    connect(&t.sigTimeout, [task] { g_scheduler->resume(task); });
    
    t.start(ms);
    g_scheduler->yield(task);
    t.stop();
}

void jzco_waitCoroutine(const std::vector<JZCoCoroutinePtr>& co_list)
{
    
    while (true)
    {
        bool all_finish = true;
        for (int i = 0; i < co_list.size(); i++)
        {
            if (!co_list[i]->isFinish())
            {
                all_finish = false;
                break;
            }
        }
        if (all_finish)
            return;

        if(g_scheduler->isInCoroutine())
            jzco_sleep(20);
        else
        {
            jzApp->processEvents();
            JZThread::msleep(20);
        }
    }
}

void jzco_waitAllCoroutine()
{
    // 协程可能在其内部再启动协程, 不能用某一时刻的快照一次等待
    // JZCoWatch 绑定 g_scheduler, 每收到 sigFinished 就判断调度器里是否已全部结束
    JZCoWatch watch;
    watch.waitScheduler();
}

void jzco_waitCoroutineAny(const std::vector<JZCoCoroutinePtr> &co_list)
{
    if (co_list.empty())
        return;

    // 上下文相关的等待由 JZCoWatch 内部处理:
    //   协程内 -> JZCoCondition 让出(调度出去); 主线程 -> JZEventLoop 等待
    JZCoWatch watch;
    watch.waitAny(co_list);
}

void jzco_waitCoroutineAll(const std::vector<JZCoCoroutinePtr> &co_list)
{
    if (co_list.empty())
        return;

    JZCoWatch watch;
    watch.waitAll(co_list);
}

void jzco_submitAndWait(std::function<void()> func)
{
    JZCoCondition cond;
    JZCoThreadPool::instance()->addTask(&cond, func);
    cond.wait();    
}

JZCoCoroutinePtr jzco_spawn(const std::function<void()> &func, int stack_size)
{
    JZCoCoroutinePtr ptr = JZCoCoroutinePtr(new JZCoCoroutine(stack_size));    
    ptr->setTask(func);
    g_scheduler->startTask(ptr);
    return ptr;
}

void jzco_start(JZCoCoroutinePtr co_ptr)
{
    g_scheduler->startTask(co_ptr);
}