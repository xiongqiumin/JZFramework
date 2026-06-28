#include "JZGlobal.h"
#include "JZElapsedTimer.h"

#ifdef JZ_OS_NORMAL
#include <chrono>
#elif JZ_OS == JZ_OS_FREE_RTOS
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#elif JZ_OS == JZ_OS_RT_THREAD
#include <rtthread.h>
#endif

// 构造函数：初始化为无效状态
JZElapsedTimer::JZElapsedTimer()
    : m_valid(false), m_startNs(0) 
{
}

JZElapsedTimer::~JZElapsedTimer()
{
}

// 获取流逝时间（毫秒）
int64_t JZElapsedTimer::elapsed() const {
    if (!isValid()) 
        return 0;
    
    return nsecsElapsed() / 1000000; // 纳秒 -> 毫秒
}

// 判断是否超时
bool JZElapsedTimer::hasExpired(int64_t timeout) const {
    if (!isValid()) 
        return true; // 无效状态视为已过期
    
    return elapsed() >= timeout;
}

// 使计时器失效
void JZElapsedTimer::invalidate() {
    m_valid = false;
    m_startNs = 0;
}

// 检查有效性
bool JZElapsedTimer::isValid() const {
    return m_valid;
}

// 获取流逝时间（纳秒）
int64_t JZElapsedTimer::nsecsElapsed() const {
    if (!isValid()) return 0;
    return (currentTimeNs() - m_startNs);
}

// 重启计时器
int64_t JZElapsedTimer::restart() {
    int64_t prevElapsed = elapsed();
    start();
    return prevElapsed;
}

// 启动计时器
void JZElapsedTimer::start() {
    m_startNs = currentTimeNs();
    m_valid = true;
}

// 内部辅助函数：获取当前系统时间（纳秒）
int64_t JZElapsedTimer::currentTimeNs() const{
#ifdef JZ_OS_NORMAL
    // 标准C++实现（裸机/桌面系统）
    auto now = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()
        ).count();

#elif JZ_OS == JZ_OS_FREE_RTOS    
    TickType_t tick = xTaskGetTickCount();
    // Tick转纳秒：tick * (1000000000 / configTICK_RATE_HZ)
    int64_t ns = (int64_t)tick * (1000000000 / configTICK_RATE_HZ);
    return ns;

#elif JZ_OS == JZ_OS_RT_THREAD
    rt_uint32_t tick = rt_tick_get();
    // 基础版：tick转纳秒
    int64_t ns = (int64_t)tick * 1000000;
    return ns;

#else
    return 0;
#endif

}