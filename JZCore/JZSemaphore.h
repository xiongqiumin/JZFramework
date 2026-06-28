#ifndef JZ_SEMAPHORE_H_
#define JZ_SEMAPHORE_H_

#include "JZGlobal.h"
#include "JZMutex.h"
#include "JZConditionVariable.h"

class JZSemaphore
{
public:    
    explicit JZSemaphore(int n = 0);
    ~JZSemaphore();

    void acquire(int n = 1);
    int available() const;
    void release(int n = 1);
    bool tryAcquire(int n = 1);
    bool tryAcquire(int n, int timeout);

private:
    JZ_DISABLE_COPY(JZSemaphore);

    mutable JZMutex m_mutex;
    JZConditionVariable m_cv;
    int m_count;
};

#endif // !JZ_SEMAPHORE_H_