#include "JZConditionVariable.h"

// 平台相关头文件仅在实现文件中包含（头文件完全隐藏）
#ifdef JZ_OS_NORMAL
    #include <chrono>
#elif JZ_OS == JZ_OS_FREE_RTOS
    #include <freertos/FreeRTOS.h>
    #include <freertos/queue.h>    
    #include <freertos/semphr.h>
#elif JZ_OS == JZ_OS_RT_THREAD
    #include <rtthread.h>
#endif

// -------------------------- 构造函数：运行时初始化底层句柄 --------------------------
JZConditionVariable::JZConditionVariable() {
    #ifdef JZ_OS_NORMAL
        
    #elif JZ_OS == JZ_OS_FREE_RTOS
        // FreeRTOS：创建队列并绑定到 void* m_cv
        QueueHandle_t queue = xQueueCreate(1, sizeof(uint8_t));
        JZ_ASSERT(queue != NULL);

        m_cv = static_cast<void*>(queue);
    #elif JZ_OS == JZ_OS_RT_THREAD
        // RT-Thread：创建条件变量并绑定到 void* m_cv
        rt_cond_t cond = rt_cond_create("jz_cv", RT_IPC_FLAG_PRIO);
        JZ_ASSERT(cond != RT_NULL);

        m_cv = static_cast<void*>(cond);
    #endif
}

// -------------------------- 析构函数：运行时销毁底层句柄 --------------------------
JZConditionVariable::~JZConditionVariable() {
    #ifdef JZ_OS_NORMAL
        // 普通系统：自动销毁
    #elif JZ_OS == JZ_OS_FREE_RTOS
        if (m_cv != nullptr) {
            vQueueDelete(static_cast<QueueHandle_t>(m_cv));
            m_cv = nullptr;
        }
    #elif JZ_OS == JZ_OS_RT_THREAD
        if (m_cv != nullptr) {
            rt_cond_delete(static_cast<rt_cond_t>(m_cv));
            m_cv = nullptr;
        }
    #endif
}


// -------------------------- notify_one：跨平台适配 --------------------------
void JZConditionVariable::notify_one() {
#ifdef JZ_OS_NORMAL
    m_cv.notify_one();
#elif JZ_OS == JZ_OS_FREE_RTOS
    // 从 void* 转换为 FreeRTOS 队列句柄
    QueueHandle_t queue = static_cast<QueueHandle_t>(m_cv);
    uint8_t dummy = 1;
    xQueueSend(queue, &dummy, 0); // 非阻塞发送
#elif JZ_OS == JZ_OS_RT_THREAD
    // 从 void* 转换为 RT-Thread 条件变量句柄
    rt_cond_t cond = static_cast<rt_cond_t>(m_cv);
    rt_cond_signal(cond);
#endif
}

// -------------------------- notify_all：FreeRTOS 断言 --------------------------
void JZConditionVariable::notify_all() {
#ifdef JZ_OS_NORMAL
    m_cv.notify_all();
#elif JZ_OS == JZ_OS_FREE_RTOS
    JZ_ASSERT(!"FreeRTOS JZConditionVariable does NOT support notify_all()! Use notify_one() instead.");
#elif JZ_OS == JZ_OS_RT_THREAD
    rt_cond_t cond = static_cast<rt_cond_t>(m_cv);
    rt_cond_broadcast(cond);
#endif
}

// -------------------------- 基础 wait：复用 wait_for --------------------------
void JZConditionVariable::wait(JZMutexLocker& locker)
{
#ifdef JZ_OS_NORMAL
    // 普通系统：正确使用 std::unique_lock 包装 mutex（避免析构解锁）
    JZMutex* mutex = locker.mutex();
    JZ_ASSERT(mutex);
    
    std::unique_lock<std::mutex> lock(mutex->m_mutex, std::adopt_lock);        
    m_cv.wait(lock);
    lock.release();
#else
    wait_for(locker, -1);
#endif
}

void JZConditionVariable::wait(JZMutexLocker& locker,std::function<bool()> condition)
{
    wait_for(mutex, -1, condition);
}

// -------------------------- 核心：wait_for（超时 + lambda 条件） --------------------------
bool JZConditionVariable::wait_for(JZMutexLocker& locker, int timeout_ms)
{
#ifdef JZ_OS_NORMAL
    // 普通系统：正确使用 std::unique_lock 包装 mutex（避免析构解锁）
    JZMutex* mutex = locker.mutex();
    JZ_ASSERT (mutex) ;

    // 接管已加锁的 mutex（adopt_lock 表示不重复加锁）
    std::unique_lock<std::mutex> lock(mutex->m_mutex, std::adopt_lock);
	bool ret = false;
    if (timeout_ms == -1)
	{
		 m_cv.wait(lock);
		 ret = true;
	}
    else
	{
        std::chrono::milliseconds timeout = std::chrono::milliseconds(timeout_ms);
    
		// 调用 wait_for（自动解锁→等待→唤醒后加锁）
		auto status = m_cv.wait_for(lock, timeout);
		ret = (status == std::cv_status::no_timeout);
	}
    
    // 释放 lock 对 mutex 的管理权（避免析构时解锁，由 JZMutexLocker 负责解锁
    lock.release();
    return ret;

#elif JZ_OS == JZ_OS_FREE_RTOS
    // FreeRTOS：基于队列+互斥体实现条件变量超时等待
    JZMutex* mutex = locker.mutex();
    JZ_ASSERT(mutex && m_cv);

    QueueHandle_t cond_queue = static_cast<QueueHandle_t>(m_cv);
    SemaphoreHandle_t mutex_hdl = static_cast<SemaphoreHandle_t>(mutex->m_mutex);

    // 转换超时时间（UINT32_MAX 表示永久等待）
    TickType_t timeout_ticks = (timeout_ms == -1) ? portMAX_DELAY
        : pdMS_TO_TICKS(timeout_ms);
    // 兜底：避免 pdMS_TO_TICKS 返回 0 导致无等待
    if (timeout_ms != -1 && timeout_ticks == 0) {
        timeout_ticks = 1;
    }
    
    TickType_t remaining_tick = timeout_ticks;
    
    // 2. 解锁互斥体（必须检查返回值，避免解锁失败）
    BaseType_t give_ret = xSemaphoreGive(mutex_hdl);
    if (give_ret != pdTRUE) {
        JZ_ASSERT(false && "Failed to unlock FreeRTOS mutex");
        return false;
    }

    xQueueReset(cond_queue);

    // 3. 等待队列信号（带剩余超时）
    uint8_t dummy;    
    BaseType_t queue_ret = xQueueReceive(cond_queue, &dummy, remaining_tick);

    // 4. 重新加锁互斥体（永久等待，确保加锁成功）
    BaseType_t take_ret = xSemaphoreTake(mutex_hdl, portMAX_DELAY);
    JZ_ASSERT(take_ret == pdTRUE);

    // 5. 处理队列等待结果
    if (queue_ret == pdTRUE) {
        return true;
    }
    else {
        // 超时/队列错误：最终检查条件
        return false;
    }    

#elif JZ_OS == JZ_OS_RT_THREAD
    // RT-Thread：基于原生条件变量实现
    JZMutex* mutex = locker.mutex();
    JZ_ASSERT(mutex && m_cv);

    rt_cond_t cond = static_cast<rt_cond_t>(m_cv);
    rt_mutex_t mutex_hdl = static_cast<rt_mutex_t>(mutex->m_mutex);

    // 转换超时时间（UINT32_MAX 表示永久等待）
    rt_int32_t timeout_ticks = (timeout_ms == -1) ? RT_WAITING_FOREVER
        : rt_ms_to_tick(timeout_ms);
    
    // 2. 等待条件变量（自动解锁/加锁）
    rt_err_t ret = rt_cond_wait(cond, mutex_hdl, timeout_ticks);

    // 3. 处理等待结果
    switch (ret) {
    case RT_EOK:
        return true;
    case -RT_ETIMEOUT:
        // 超时：最终检查条件
        return condition();
    default:
        // 其他错误（如 RT_ERROR/RT_EINVAL）：返回失败
        JZ_ASSERT(0 && "rt_cond_wait failed, err=" + ret);
        return false;
    }    
#endif

    // 兜底返回（理论上不会执行到）
    return false;
}

bool JZConditionVariable::wait_for(JZMutexLocker& locker, int timeout_ms, std::function<bool()> condition) {
    // 前置检查：条件为空直接返回失败
    JZ_ASSERT(condition);

#ifdef JZ_OS_NORMAL
    // 普通系统：正确使用 std::unique_lock 包装 mutex（避免析构解锁）
    JZMutex* mutex = locker.mutex();
    JZ_ASSERT (mutex) ;

    // 接管已加锁的 mutex（adopt_lock 表示不重复加锁）
    std::unique_lock<std::mutex> lock(mutex->m_mutex, std::adopt_lock);
    bool cond_met = false;

    // 调用 wait_for（自动解锁→等待→唤醒后加锁）
    if (timeout_ms == -1)
    {
        m_cv.wait(lock, condition);
        cond_met = true; 
    }
    else
    {
        std::chrono::milliseconds timeout = std::chrono::milliseconds(timeout_ms);
        cond_met = m_cv.wait_for(lock, timeout, condition);
    }
    
    // 释放 lock 对 mutex 的管理权（避免析构时解锁，由 JZMutexLocker 负责解锁）
    lock.release();
    return cond_met;

#elif JZ_OS == JZ_OS_FREE_RTOS
    // FreeRTOS：基于队列+互斥体实现条件变量超时等待
    JZMutex* mutex = locker.mutex();
    JZ_ASSERT (mutex && m_cv) ;

    QueueHandle_t cond_queue = static_cast<QueueHandle_t>(m_cv);
    SemaphoreHandle_t mutex_hdl = static_cast<SemaphoreHandle_t>(mutex->m_mutex);

    // 转换超时时间（UINT32_MAX 表示永久等待）
    TickType_t timeout_ticks = (timeout_ms == -1)? portMAX_DELAY 
                                : pdMS_TO_TICKS(timeout_ms);
    // 兜底：避免 pdMS_TO_TICKS 返回 0 导致无等待
    if (timeout_ms != -1 && timeout_ticks == 0) {
        timeout_ticks = 1;
    }

    TickType_t start_tick = xTaskGetTickCount();
    TickType_t remaining_tick = timeout_ticks;

    xQueueReset(cond_queue);
    while (true) {
        // 1. 优先检查条件：满足则直接返回
        if (condition()) {
            return true;
        }

        // 2. 解锁互斥体（必须检查返回值，避免解锁失败）
        BaseType_t give_ret = xSemaphoreGive(mutex_hdl);
        JZ_ASSERT(give_ret == pdTRUE);

        // 3. 等待队列信号（带剩余超时）
        uint8_t dummy;
        BaseType_t queue_ret = xQueueReceive(cond_queue, &dummy, remaining_tick);

        // 4. 重新加锁互斥体（永久等待，确保加锁成功）
        BaseType_t take_ret = xSemaphoreTake(mutex_hdl, portMAX_DELAY);
        JZ_ASSERT(take_ret == pdTRUE);
         
        // 5. 处理队列等待结果
        if (queue_ret == pdTRUE) {
            // 被 notify_one 唤醒：更新剩余超时（防无符号数溢出）
            if (timeout_ms != -1) {
                TickType_t elapsed = xTaskGetTickCount() - start_tick;
                remaining_tick = (elapsed < remaining_tick) ? (remaining_tick - elapsed) : 0;
            }
        } else {
            // 超时/队列错误：最终检查条件
            return condition();
        }
    }

#elif JZ_OS == JZ_OS_RT_THREAD
    // RT-Thread：基于原生条件变量实现
    JZMutex* mutex = locker.mutex();
    JZ_ASSERT (mutex && m_cv) ;

    rt_cond_t cond = static_cast<rt_cond_t>(m_cv);
    rt_mutex_t mutex_hdl = static_cast<rt_mutex_t>(mutex->m_mutex);

    // 转换超时时间（UINT32_MAX 表示永久等待）
    rt_int32_t timeout_ticks = (timeout_ms == -1)? RT_WAITING_FOREVER 
                                : rt_ms_to_tick(timeout_ms);

    while (true) {
        // 1. 优先检查条件
        if (condition()) {
            return true;
        }

        // 2. 等待条件变量（自动解锁/加锁）
        rt_err_t ret = rt_cond_wait(cond, mutex_hdl, timeout_ticks);

        // 3. 处理等待结果
        switch (ret) {
            case RT_EOK:
                // 被唤醒：重新检查条件
                continue;
            case -RT_ETIMEOUT:
                // 超时：最终检查条件
                return condition();
            default:
                // 其他错误（如 RT_ERROR/RT_EINVAL）：返回失败
                JZ_ASSERT(0 && "rt_cond_wait failed, err=" + ret);
                return false;
        }
    }
#endif

    // 兜底返回（理论上不会执行到）
    return false;
}