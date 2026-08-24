#include "JZGlobal.h"
#include "JZElapsedTimer.h"
#include "JZPlatform.h"

#ifdef JZ_OS_NORMAL
#include <chrono>
#elif JZ_OS == JZ_OS_FREE_RTOS
#include "JZCore/private/JZFreeRTOS.h"
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
int64_t JZElapsedTimer::currentTimeNs() const
{
    return JZPlatform::getTickCountNs();
}