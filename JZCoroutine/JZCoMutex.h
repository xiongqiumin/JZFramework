#ifndef JZ_CO_MUTEX_H_
#define JZ_CO_MUTEX_H_
#include <queue>
#include "JZGlobal.h"
#include "JZCoCoroutine.h"

class JZCoCoroutine;
class JZFRAMEWORK_API JZCoMutex
{
public:
    JZCoMutex();
    ~JZCoMutex();

    bool tryLock();
    void lock();
    void unlock();

protected:
    JZ_DISABLE_COPY(JZCoMutex);

    bool m_locked;
    JZCoCondition m_cond;
};

//JZCoMutexLocker
class JZFRAMEWORK_API JZCoMutexLocker
{
public:
    JZCoMutexLocker(JZCoMutex *mutex);
    ~JZCoMutexLocker();

protected:
    JZCoMutex *m_mutex;
};

#endif