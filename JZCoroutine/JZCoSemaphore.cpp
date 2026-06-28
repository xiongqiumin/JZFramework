// JZCoSemaphore.cpp
#include "JZCoSemaphore.h"
#include "JZCo.h"
#include "JZDebug.h"
#include "JZTimer.h"

JZCoSemaphore::JZCoSemaphore(int n)
    : m_count(n)
{
}

JZCoSemaphore::~JZCoSemaphore()
{
}

int JZCoSemaphore::available() const
{
    return m_count;
}

bool JZCoSemaphore::tryAcquire(int n)
{
    if (n <= 0)
        return true;
        
    if (m_count >= n) {
        m_count -= n;
        return true;
    }
    return false;
}

void JZCoSemaphore::acquire(int n)
{
    if (n <= 0)
        return;
        
    // 快速路径：资源充足
    if (m_count >= n) {
        m_count -= n;
        return;
    }
    
    // 资源不足，挂起当前协程
    auto co = g_scheduler->currentCoroutine();
    m_waitList.push(WaitNode(co, n));
    
    // 挂起协程（会被 release 唤醒）
    g_scheduler->yield(co);
}

bool JZCoSemaphore::tryAcquire(int n, int timeout)
{
    if (n <= 0)
        return true;
        
    if (timeout < 0)
        return tryAcquire(n);
        
    // 快速路径：资源充足
    if (m_count >= n) {
        m_count -= n;
        return true;
    }
    
    // 资源不足，使用带超时的等待
    auto co = g_scheduler->currentCoroutine();
    
    // 加入等待队列
    m_waitList.push(WaitNode(co, n));
    
    // 使用 jzco_sleep 等待，可能被两种情况唤醒：
    // 1. 超时唤醒
    // 2. release 提前唤醒
    jzco_sleep(timeout);
    
    // 被唤醒后检查是否成功获取到资源
    if (m_count >= n) {
        // 成功获取资源
        m_count -= n;
        // 从等待队列中移除自己
        removeFromWaitList(co);
        return true;
    } else {
        // 超时失败，从等待队列中移除自己
        removeFromWaitList(co);
        return false;
    }
}

void JZCoSemaphore::release(int n)
{
    if (n <= 0)
        return;
        
    m_count += n;

    wakeupWaiting();
}

void JZCoSemaphore::wakeupWaiting()
{
    // 收集所有需要唤醒的协程
    std::vector<JZCoCoroutine*> toResume;
    std::queue<WaitNode> newWaitList;
    
    while (!m_waitList.empty()) {
        auto& node = m_waitList.front();
        
        // 检查是否能满足需求
        if (m_count >= node.needCount) {
            m_count -= node.needCount;
            toResume.push_back(node.co);
            m_waitList.pop();
        } else {
            newWaitList.push(node);
            m_waitList.pop();
        }
    }
    m_waitList.swap(newWaitList);
    
    // 延迟到下一个事件循环唤醒，避免递归和栈溢出
    if (!toResume.empty()) 
    {
        JZTimer::singleShot(0, [toResume = std::move(toResume)]() mutable {
            for (auto co : toResume) {
                g_scheduler->resume(co);
            }
        });
    }
}

void JZCoSemaphore::removeFromWaitList(JZCoCoroutine* co)
{
    std::queue<WaitNode> newQueue;
    while (!m_waitList.empty()) {
        auto& node = m_waitList.front();
        if (node.co != co) {
            newQueue.push(node);
        }
        m_waitList.pop();
    }
    m_waitList.swap(newQueue);
}