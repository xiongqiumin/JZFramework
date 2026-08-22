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

#if JZ_OS == JZ_OS_FREE_RTOS

class FreeRtosCv {

public:
    FreeRtosCv() : event_group_(xEventGroupCreate()), mutex_(xSemaphoreCreateMutex()) {
        available_bits_ = (1 << MAX_BITS) - 1;
    }

    ~FreeRtosCv() { vEventGroupDelete(event_group_); vSemaphoreDelete(mutex_); }

    bool wait_for(SemaphoreHandle_t lock, int timeout_ms)
    {
        bool first = true;
        return wait_for(lock, timeout_ms, [&first]{
            if(first)
            {
                first = false;
                return false;
            }
            else
                return true;
        });
    }

    bool wait_for(SemaphoreHandle_t lock, int timeout_ms, const std::function<bool()>& cond) 
    {
        while (!cond()) {
            uint32_t my_bit = allocate_bit();
            if (!my_bit) 
            {
                JZ_ASSERT_X(0, "to many wait for");
                return false;
            }

            TaskHandle_t self = xTaskGetCurrentTaskHandle();

            // 入队
            xSemaphoreTake(mutex_, portMAX_DELAY);
            waiters_.push_back({self, my_bit});
            xSemaphoreGive(mutex_);

            // 释放锁，等待事件
            xSemaphoreGive(lock);
            TickType_t ticks = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
            if (timeout_ms > 0 && ticks == 0) ticks = 1;   // [修复] 100Hz tick 下 <10ms 正超时被截断为 0 -> 忙轮询, 提升为最小 1 tick
            EventBits_t triggered = xEventGroupWaitBits(event_group_, my_bit, pdTRUE, pdFALSE, ticks);
            xSemaphoreTake(lock, portMAX_DELAY);

            // 出队
            remove_self(self, my_bit);
            free_bit(my_bit);

            if ((triggered & my_bit) == 0) {
                return false; // 超时
            }
            // 唤醒后循环检查条件
        }
        return true;
    }

    void notify_one() {
        xSemaphoreTake(mutex_, portMAX_DELAY);
        if (waiters_.empty()) { xSemaphoreGive(mutex_); return; }
        uint32_t bit = waiters_.front().bit_mask; // 取队首
        xSemaphoreGive(mutex_);
        xEventGroupSetBits(event_group_, bit);
    }

    void notify_all() {
        xSemaphoreTake(mutex_, portMAX_DELAY);
        if (waiters_.empty()) { 
            xSemaphoreGive(mutex_); 
            return; 
        }
        
        EventBits_t bits = 0;
        for (auto& w : waiters_) 
            bits |= w.bit_mask;
        xSemaphoreGive(mutex_);
        xEventGroupSetBits(event_group_, bits);
    }

private:
    struct WaitingTask {
        TaskHandle_t handle;
        uint32_t bit_mask;
    };

    uint32_t allocate_bit() {
        xSemaphoreTake(mutex_, portMAX_DELAY);
        if (available_bits_ == 0) { xSemaphoreGive(mutex_); return 0; }
        uint32_t bit = available_bits_ & -available_bits_;
        available_bits_ &= ~bit;
        xSemaphoreGive(mutex_);
        return bit;
    }

    void free_bit(uint32_t bit) {
        xSemaphoreTake(mutex_, portMAX_DELAY);
        available_bits_ |= bit;
        xSemaphoreGive(mutex_);
    }

    void remove_self(TaskHandle_t self, uint32_t bit) {
        xSemaphoreTake(mutex_, portMAX_DELAY);
        for (auto it = waiters_.begin(); it != waiters_.end(); ++it) {
            if (it->handle == self && it->bit_mask == bit) {
                waiters_.erase(it);
                break;
            }
        }
        xSemaphoreGive(mutex_);
    }    

    
    static constexpr int MAX_BITS = 24;
    EventGroupHandle_t event_group_;
    SemaphoreHandle_t mutex_;
    std::vector<WaitingTask> waiters_;
    uint32_t available_bits_;
};

#endif

// -------------------------- 构造函数：运行时初始化底层句柄 --------------------------
JZConditionVariable::JZConditionVariable() {
    #ifdef JZ_OS_NORMAL
        
    #elif JZ_OS == JZ_OS_FREE_RTOS        
        m_cv = new FreeRtosCv();
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
    FreeRtosCv *cv = (FreeRtosCv *)m_cv;
    return cv->notify_one();
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
    FreeRtosCv *cv = (FreeRtosCv *)m_cv;
    return cv->notify_all();
#elif JZ_OS == JZ_OS_RT_THREAD
    rt_cond_t cond = static_cast<rt_cond_t>(m_cv);
    rt_cond_broadcast(cond);
#endif
}

// -------------------------- 基础 wait：复用 wait_for --------------------------
void JZConditionVariable::wait(JZMutexLocker& locker)
{   
    wait_for(locker, -1);
}

void JZConditionVariable::wait(JZMutexLocker& locker,const std::function<bool()>& condition)
{
    wait_for(locker, -1, condition);
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
    FreeRtosCv *cv = (FreeRtosCv *)m_cv;
    SemaphoreHandle_t handle = (SemaphoreHandle_t)locker.mutex()->m_mutex;
    return cv->wait_for(handle, timeout_ms);

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

bool JZConditionVariable::wait_for(JZMutexLocker& locker, int timeout_ms, const std::function<bool()>& condition) {
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
    FreeRtosCv *cv = (FreeRtosCv *)m_cv;
    SemaphoreHandle_t handle = (SemaphoreHandle_t)locker.mutex()->m_mutex;
    return cv->wait_for(handle, timeout_ms, condition);

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