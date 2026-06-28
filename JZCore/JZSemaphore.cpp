#include "JZSemaphore.h"

JZSemaphore::JZSemaphore(int n) 
    :m_count(n) 
{
}

JZSemaphore::~JZSemaphore()
{
}

void JZSemaphore::acquire(int n)
{
    JZMutexLocker lock(&m_mutex);
    m_cv.wait(lock, [this, n]()->bool {
        return m_count >= n; 
    });
    m_count -= n;
}

int JZSemaphore::available() const
{
    JZMutexLocker lock(&m_mutex);
    return m_count;
}

void JZSemaphore::release(int n)
{
    {
        JZMutexLocker lock(&m_mutex);
        m_count += n;
    }
    for (int i = 0; i < n; ++i)
    {
        m_cv.notify_one();
    }
}

bool JZSemaphore::tryAcquire(int n)
{
    JZMutexLocker lock(&m_mutex);
    if (m_count >= n)
    {
        m_count -= n;
        return true;
    }
    return false;
}

bool JZSemaphore::tryAcquire(int n, int timeout)
{
    JZMutexLocker lock(&m_mutex);
    if (m_cv.wait_for(lock, timeout,
        [this, n]() { return m_count >= n; }))
    {
        m_count -= n;
        return true;
    }
    return false;
}