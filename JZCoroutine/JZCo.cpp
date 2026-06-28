#include "JZCo.h"
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

void jzco_waitThread(std::function<void()> func)
{
    JZCoThread t(func);
    t.start();
    while (true)
    {
        if (t.isRunning())
            jzco_sleep(10);
        else
            break;
    }
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
    auto co_list = g_scheduler->coroutineList();
    jzco_waitCoroutine(co_list);
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