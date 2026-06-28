#include "JZTimer.h"
#include "JZCoCoroutine.h"
#include "JZApplication.h"
#include "JZDebug.h"

class JZCoEvent : public JZEvent
{
public:
    JZCoEvent()
        :JZEvent(JZEvent_co)
    {
    }
};

//JZCoCoroutine
JZCoCoroutine::JZCoCoroutine()
{
    m_ctx = createContext(this);
    m_interrupt = false;
    m_finish = false;
}

JZCoCoroutine::~JZCoCoroutine()
{
    JZ_ASSERT(!m_ctx); //在调度器中释放
}

std::string JZCoCoroutine::name()
{
    return m_name;
}

void JZCoCoroutine::setName(std::string name)
{
    m_name = name;
}

void JZCoCoroutine::setTask(const std::function<void()>& func)
{
    m_func = func;
}

void JZCoCoroutine::run()
{
    if(m_func)
        m_func();
}

void JZCoCoroutine::quit()
{
    m_interrupt = true;
    if(g_scheduler->currentCoroutine() == this)
        throw JZCoCoroutineInterrupt();
    else
        g_scheduler->resume(this);
}

bool JZCoCoroutine::isInterrupt() const
{
    return m_interrupt;
}

bool JZCoCoroutine::isFinish() const
{
    return m_finish;
}

void JZCoCoroutine::beforeYield()
{
}

void JZCoCoroutine::beforeResume()
{
}

void JZCoCoroutine::endTask()
{
}

//JZCoThread
JZCoThread::JZCoThread(std::function<void()> func)
{
    m_func = func;
}

void JZCoThread::run()
{
    m_func();
}

//JZCoScheduler
JZCoScheduler* g_scheduler = nullptr;
JZCoScheduler::JZCoScheduler(JZObject* parent)
    :JZObject(parent)
{
    JZ_ASSERT(!g_scheduler);

    m_mainCtx = nullptr;
    m_co = nullptr;
    g_scheduler = this;
}

JZCoScheduler::~JZCoScheduler()
{
    onClearContext();
    if(m_mainCtx)
        destoryMainContext(m_mainCtx);

    g_scheduler = nullptr;
}

void JZCoScheduler::init()
{
    m_mainCtx = createMainContext();
}

bool JZCoScheduler::isInCoroutine()
{
    return (m_co != nullptr);
}

void JZCoScheduler::yield(JZCoCoroutine* co)
{
    JZ_ASSERT(co && co == m_co);
    co->beforeYield();

    CO_CONTEXT_HANDLE cur = getCoContextHandle(co->m_ctx);
    CO_CONTEXT_HANDLE next = getMainContextHandle(g_scheduler->mainContext());

    m_co = nullptr;
    switchCoroutine(cur, next);
}

void JZCoScheduler::resume(JZCoCoroutine* co)
{
    JZ_ASSERT(co && !m_co);

    co->beforeResume();

    CO_CONTEXT_HANDLE cur;
    if (m_co)
        cur = getCoContextHandle(m_co->m_ctx);
    else
        cur = getMainContextHandle(g_scheduler->mainContext());

    CO_CONTEXT_HANDLE next = getCoContextHandle(co->m_ctx);
    
    m_co = co;
    switchCoroutine(cur, next);
}

void JZCoScheduler::startTask(JZCoCoroutinePtr task)
{
    task->m_finish = false;
    m_coList.push_back(task);

    JZTimer::singleShot(0, this, [this, task] {
        JZ_ASSERT(!task->m_finish);
        resume(task.get());
    });
}

void JZCoScheduler::endTask(JZCoCoroutine *task)
{
    JZ_ASSERT(task == m_co);
    m_co = nullptr;

    auto ctx = task->m_ctx;
    m_finished.push_back(ctx);

    CO_CONTEXT_HANDLE cur = getCoContextHandle(task->m_ctx);
    CO_CONTEXT_HANDLE next = getMainContextHandle(g_scheduler->mainContext());

    task->m_finish = true;
    task->m_ctx = nullptr;
    for (int i = 0; i < m_coList.size(); i++)
    {
        if (m_coList[i].get() == task)
        {
            task->endTask();
            vec_removeAt(m_coList,i);
            break;
        }
    }

    jzApp->postEvent(this,new JZCoEvent());
    switchCoroutine(cur,next);
}

JZCoMainContext* JZCoScheduler::mainContext()
{
    return m_mainCtx;
}

JZCoCoroutine* JZCoScheduler::currentCoroutine()
{
    return m_co;
}

std::vector<JZCoCoroutinePtr> JZCoScheduler::coroutineList()
{
    return m_coList;
}

void JZCoScheduler::onClearContext()
{
    for (int i = 0; i < m_finished.size(); i++)
        destoryContext(m_finished[i]);
    m_finished.clear();
}

bool JZCoScheduler::event(JZEvent* event)
{
    if (event->type() == JZEvent_co)
    {
        onClearContext();
        return true;
    }

    return JZObject::event(event);
}

//JZCoCondition
JZCoCondition::JZCoCondition()
{
}

void JZCoCondition::wait(int timeout)
{
    JZTimer t;
    t.setSingleShot(true);

    JZCoCoroutine* co = g_scheduler->currentCoroutine();
    m_coList.push_back(co);
    if (timeout != -1)
    {
        connect(&t.sigTimeout, [this, co] {
            g_scheduler->resume(co);
        });
        t.start(timeout);
    }
    g_scheduler->yield(co);
    vec_removeOne(m_coList, co);
    t.stop();
}

void JZCoCondition::wakeOne()
{
    if (m_coList.size() == 0)
        return;

    JZCoCoroutine* co = m_coList.front();
    vec_popFront(m_coList);
    g_scheduler->resume(co);
}

void JZCoCondition::wakeAll()
{
    std::vector<JZCoCoroutine*> co_list = m_coList;
    m_coList.clear();

    for (int i = 0; i < (int)co_list.size(); i++)
    {
        g_scheduler->resume(co_list[i]);
    }
}