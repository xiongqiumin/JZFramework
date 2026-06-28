#include "JZCoMutex.h"
#include "JZCo.h"

//JZCoMutex
JZCoMutex::JZCoMutex() 
    :m_locked(false)
{
}

JZCoMutex::~JZCoMutex()
{
    JZ_ASSERT(!m_locked);
}

bool JZCoMutex::tryLock()
{
    if (m_locked)
        return false;

    m_locked = true;
    return true;
}

void JZCoMutex::lock()
{
    if (!m_locked)
    {
        m_locked = true;
        return;
    }

    m_coWaitList.push(g_scheduler->currentCoroutine());
    g_scheduler->yield(m_coWaitList.back());
}

void JZCoMutex::unlock()
{
    if (m_coWaitList.empty())
    {
        m_locked = false;
        return;
    }

    JZCoCoroutine* co = m_coWaitList.front();
    m_coWaitList.pop();
    g_scheduler->resume(co);
}

//JZCoMutexLocker
JZCoMutexLocker::JZCoMutexLocker(JZCoMutex *mutex)
{
    m_mutex = mutex;
    m_mutex->lock();
}

JZCoMutexLocker::~JZCoMutexLocker()
{
    m_mutex->unlock();
}