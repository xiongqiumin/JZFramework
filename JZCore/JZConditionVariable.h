#ifndef JZ_CONDITION_VARIABLE 
#define JZ_CONDITION_VARIABLE 

#include "JZGlobal.h"
#include "JZMutex.h"
#include <functional> // 仅保留 std::function（跨平台）

#ifdef JZ_OS_NORMAL
#include <condition_variable>
#endif

class JZConditionVariable
{
public:
    JZConditionVariable();
    ~JZConditionVariable();

    // 基础接口
    void wait(JZMutexLocker& locker);
    void wait(JZMutexLocker& locker,std::function<bool()> condition);

    bool wait_for(JZMutexLocker& locker, int timeout_ms);
    bool wait_for(JZMutexLocker& locker, int timeout_ms, std::function<bool()> condition);

    void notify_one();
    void notify_all();

protected:
    JZ_DISABLE_COPY(JZConditionVariable)
    
private:
    // 统一封装条件变量：普通系统用原生类型，其他平台用 void*
    #ifdef JZ_OS_NORMAL
        std::condition_variable m_cv;
    #else
        void* m_cv; // 运行时绑定 FreeRTOS 队列/RT-Thread 条件变量
    #endif
};


#endif