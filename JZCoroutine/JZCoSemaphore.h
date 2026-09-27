#ifndef JZCO_SEMAPHORE_H_
#define JZCO_SEMAPHORE_H_
#include <queue>
#include "JZGlobal.h"

class JZCoCoroutine;
class JZFRAMEWORK_API JZCoSemaphore
{
public:    
    explicit JZCoSemaphore(int n = 0);
    ~JZCoSemaphore();

    void acquire(int n = 1);
    int available() const;
    void release(int n = 1);
    bool tryAcquire(int n = 1);
    bool tryAcquire(int n, int timeout);

private:    
    struct WaitNode {
        JZCoCoroutine* co;
        int needCount;
        
        WaitNode(JZCoCoroutine* c, int need)
            : co(c), needCount(need) {}
    };

    int m_count;
    std::queue<WaitNode> m_waitList;
    
    void wakeupWaiting();
    void removeFromWaitList(JZCoCoroutine* co);
};

#endif