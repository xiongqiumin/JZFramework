#include <assert.h>
#include "JZMutex.h"

#if JZ_OS == JZ_OS_FREE_RTOS    
    #include <freertos/FreeRTOS.h>
    #include <freertos/semphr.h>
    #define MUTEX_FREERTOS ((SemaphoreHandle_t)m_mutex)
#elif JZ_OS == JZ_OS_RT_THREAD    
    #include <rtthread.h>
    #define MUTEX_RTTHREAD ((rt_mutex_t)m_mutex)
#endif

JZMutex::JZMutex() {
    #if JZ_OS == JZ_OS_FREE_RTOS        
        m_mutex = (void*)xSemaphoreCreateMutex();
        JZ_ASSERT(m_mutex);
    #elif JZ_OS == JZ_OS_RT_THREAD        
        m_mutex = (void*)rt_mutex_create(RT_NULL, RT_IPC_FLAG_PRIO);
        JZ_ASSERT(m_mutex);
    #endif
}


JZMutex::~JZMutex() {
    #if JZ_OS == JZ_OS_FREE_RTOS
        if (m_mutex != NULL) {
            vSemaphoreDelete(MUTEX_FREERTOS);
            m_mutex = NULL;
        }
    #elif JZ_OS == JZ_OS_RT_THREAD
        if (m_mutex != RT_NULL) {
            rt_mutex_delete(MUTEX_RTTHREAD);
            m_mutex = RT_NULL;
        }
    #endif
}

// -------------------------- 加锁：适配不同系统 --------------------------
void JZMutex::lock() {
    #ifdef JZ_OS_NORMAL
        m_mutex.lock(); // std::mutex 原生调用
    #elif JZ_OS == JZ_OS_FREE_RTOS        
        xSemaphoreTake(MUTEX_FREERTOS, portMAX_DELAY);
    #elif JZ_OS == JZ_OS_RT_THREAD        
        rt_mutex_take(MUTEX_RTTHREAD, RT_WAITING_FOREVER);
    #endif
}

void JZMutex::unlock() {    
    #ifdef JZ_OS_NORMAL
        m_mutex.unlock();
    #elif JZ_OS == JZ_OS_FREE_RTOS
        xSemaphoreGive(MUTEX_FREERTOS);
    #elif JZ_OS == JZ_OS_RT_THREAD
        rt_mutex_release(MUTEX_RTTHREAD);        
    #endif
}

bool JZMutex::tryLock() {
    #ifdef JZ_OS_NORMAL
        return m_mutex.try_lock();
    #elif JZ_OS == JZ_OS_FREE_RTOS        
        return xSemaphoreTake(MUTEX_FREERTOS, 0) == pdTRUE;
    #elif JZ_OS == JZ_OS_RT_THREAD        
        return rt_mutex_take(MUTEX_RTTHREAD, 0) == RT_EOK;
    #else
        return false;
    #endif
}

//JZMutexLocker
JZMutexLocker::JZMutexLocker(JZMutex *mutex)
    : m_mutex(mutex)
{
    JZ_ASSERT(m_mutex);
    m_mutex->lock();    
    m_locked = true;
}

JZMutexLocker::~JZMutexLocker()
{
    if(m_locked)
        m_mutex->unlock();
}

JZMutex *JZMutexLocker::mutex() const
{
    return m_mutex;
}

void JZMutexLocker::relock()
{
    m_mutex->lock();
    m_locked = true;
}

void JZMutexLocker::unlock()
{    
    m_mutex->unlock();
    m_locked = false;
}