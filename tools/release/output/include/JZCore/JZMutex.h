#ifndef JZ_MUTEX_H_
#define JZ_MUTEX_H_

#include "JZGlobal.h"

#ifdef JZ_OS_NORMAL
#include <mutex>
#endif

class JZConditionVariable;
class JZMutex
{
public:
    JZMutex();
    ~JZMutex();

    void lock();
    void unlock();

    bool tryLock();

protected:
    JZ_DISABLE_COPY(JZMutex)
    
private:
    friend JZConditionVariable;
    
#ifdef JZ_OS_NORMAL
    std::mutex m_mutex;
#else
    void *m_mutex;
#endif
};

class JZMutexLocker
{
public:
    explicit JZMutexLocker(JZMutex *mutex);
    ~JZMutexLocker();

    JZMutex *mutex() const;
    void relock();
    void unlock();

protected:
    JZ_DISABLE_COPY(JZMutexLocker)
    
private:
    JZMutex *m_mutex;
    bool m_locked;
};

#endif // ! JZ_MUTEX_H_
