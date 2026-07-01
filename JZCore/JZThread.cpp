#include "JZThread.h"
#include "JZStd.h"
#include "JZPlatform.h"
#include "JZApplication.h"
#include "JZLog.h"
#include "JZTx.h"
#include "JZDebug.h"

// 平台相关头文件仅在实现文件中包含
#if JZ_OS == JZ_OS_FREE_RTOS
    #include <freertos/FreeRTOS.h>
    #include <freertos/task.h>
    #include <freertos/semphr.h>
#elif JZ_OS == JZ_OS_RT_THREAD
    #include "rtthread.h"
    #define JZ_THREAD_STACK_SIZE 4096
    #define JZ_THREAD_PRIORITY   (RT_THREAD_PRIORITY_MAX/2)
#endif

//JZEventInfo
JZEventInfo::JZEventInfo()
{
    event = nullptr;
    sem = nullptr;
}

JZEventInfo::JZEventInfo(JZObject *obj, JZEvent *e)
{
    object = obj->guardPointer();
    event = e;
    sem = nullptr;
}

//JZThread
JZThread::TimerInfo::TimerInfo()
{
    object = nullptr;
    id = INVAILD_TIMER_ID;
    intervalMs = 0;
    nextTrigger = 0;
}

void JZThread::msleep(int ms)
{
#ifdef JZ_OS_NORMAL
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
#elif JZ_OS == JZ_OS_FREE_RTOS
    TickType_t ticks = pdMS_TO_TICKS(ms);
    vTaskDelay(ticks);
#elif JZ_OS == JZ_OS_RT_THREAD
    rt_thread_mdelay(ms);
#endif
}

JZThread* JZThread::currentThread()
{
    return jzApp->currentThread();
}

JZThreadId JZThread::currentThreadId()
{
#ifdef JZ_OS_NORMAL            
    return std::this_thread::get_id();
#elif JZ_OS == JZ_OS_FREE_RTOS
    return xTaskGetCurrentTaskHandle();
#elif JZ_OS == JZ_OS_RT_THREAD
    return rt_thread_self();
#else
    return nullptr;
#endif
}

JZThread::JZThread(JZObject *parent)
    :JZObject(parent), sigStarted(this), sigFinished(this)
{    
    initParam();
}

JZThread::JZThread(JZObject::MainThreadInit init)
    :JZObject(init), sigStarted(this), sigFinished(this)
{
    initParam();
    m_thread = this;
}

JZThread::~JZThread()
{        
    //thread 无效了,这里要特殊处理下,先删除
    std::vector<JZObject*> childs = m_childs;
	for (int i = 0; i < (int)childs.size(); i++)
		delete childs[i];
	m_childs.clear();
    
    m_thread->cancelObjectTimers(this);

	if (m_thread == this)
		m_thread = nullptr;
}

void JZThread::initParam()
{
    m_running = false;
    m_nextTimerId = 1;

#if JZ_OS == JZ_OS_FREE_RTOS
    m_handle = nullptr;
    m_waitSem = nullptr;
#elif JZ_OS == JZ_OS_RT_THREAD
    m_handle = nullptr;
#endif
}

void JZThread::setConfig(JZThreadConfig config)
{
    m_config = config;        
}

JZThreadConfig JZThread::config() const
{
    return m_config;
}

bool JZThread::isRunning() const
{
#ifdef JZ_OS_NORMAL        
    return m_handle.joinable();
#else
    return (m_handle != nullptr);
#endif
}


JZThreadId JZThread::id()
{
#ifdef JZ_OS_NORMAL
    return m_tid;
#else
    return m_handle;
#endif
}

void JZThread::attachStdThread(JZThreadId id)
{
#ifdef JZ_OS_NORMAL    
    m_tid = id;
#else
    m_handle = id;
#endif
    jzApp->addThread(this);
}

void JZThread::detachStdThread()
{
#ifdef JZ_OS_NORMAL       
    m_tid = THREAD_ID_NONE;   
#else
    m_handle = nullptr;
#endif    
}

bool JZThread::isRequestInterruption() const
{
    return !m_running;
}

void JZThread::requestInterruption()
{
    m_running = false;
    m_cv.notify_one();
}

void JZThread::start()
{
    assert(!m_running);
    
    m_running = true;
    jzApp->addThread(this);

#ifdef JZ_OS_NORMAL           
    m_handle = std::thread([this]{
        jzApp->addThread(this);
        sigStarted.emit();
        this->run();
        m_running = false;
        sigFinished.emit();
        jzApp->removeThread(this);
    });
    m_tid = m_handle.get_id();

#elif JZ_OS == JZ_OS_FREE_RTOS
    // FreeRTOS：创建任务
    m_waitSem = xSemaphoreCreateBinary();

    int pri = 0;
    if(m_config.priority == -1)
        pri = uxTaskPriorityGet(NULL);
    else
    {
        // 1. 先钳位用户输入到 0~16 的合法范围
        int userPri = m_config.priority;
        if (userPri < 0) userPri = 0;
        if (userPri > 16) userPri = 16;

        // 2. 线性映射到 FreeRTOS 合法范围: 0 ~ (configMAX_PRIORITIES - 1)
        UBaseType_t maxSysPri = configMAX_PRIORITIES - 1;
        pri = static_cast<UBaseType_t>((userPri * maxSysPri) / 16);
    }

    TaskHandle_t handle;
    BaseType_t ret = xTaskCreate(
        // 任务入口函数（静态函数，通过参数传递 this 指针）
        [](void* arg) {
            JZThread* thread = static_cast<JZThread*>(arg);                                    
            thread->run();            
            thread->m_running = false;
            jzApp->removeThread(thread);            

            xSemaphoreGive((SemaphoreHandle_t)thread->m_waitSem);            
            vTaskDelete(NULL);
        },
        m_name.c_str(),
        m_config.stackSize / 4,
        this,
        pri,
        &handle
    );        
    JZ_ASSERT(ret == pdPASS);
    m_handle = handle;
    jzDebug() << "create thread" << m_handle;
#elif JZ_OS == JZ_OS_RT_THREAD
    // RT-Thread：创建动态任务
    rt_thread_t task = rt_thread_create(
        // 任务名称
        "jz_thread",
        // 任务入口函数
        [](void* arg) {
            JZThread* thread = static_cast<JZThread*>(arg);
            // 注册任务到 jzApp
            jzApp->addThread(thread);
            // 执行任务入口函数
            thread->run();
            // 任务退出：更新状态 + 从 jzApp 移除
            thread->m_running = false;
            jzApp->removeThread(thread);     
            
        },
        // 传递 this 指针
        this,
        // 栈大小
        JZ_THREAD_STACK_SIZE,
        // 优先级
        JZ_THREAD_PRIORITY,
        // 时间片
        10
    );
    // 检查任务创建是否成功
    assert(task != RT_NULL);
    // 启动任务
    rt_thread_startup(task);
    // 保存任务句柄和 ID
    m_handle = task;
#endif
}

void JZThread::quit()
{
    requestInterruption();   
}

void JZThread::wait()
{
#ifdef JZ_OS_NORMAL            
    if(m_handle.joinable())
        m_handle.join();
    m_handle = std::thread();
#elif JZ_OS == JZ_OS_FREE_RTOS                   
    // FreeRTOS 任务退出后需删除自身
    if (m_handle != nullptr) {
        xSemaphoreTake((SemaphoreHandle_t)m_waitSem, portMAX_DELAY);               
        vSemaphoreDelete((SemaphoreHandle_t)m_waitSem);
        m_waitSem = nullptr;        
        m_handle = nullptr;
    }
#elif JZ_OS == JZ_OS_RT_THREAD        
    if (m_handle != nullptr) {
        rt_thread_t taskHdl = static_cast<rt_thread_t>(m_handle);
        // 阻塞等待任务结束
        rt_thread_join(taskHdl);
        // 统一外部删除任务
        rt_thread_delete(taskHdl);
        m_handle = nullptr;
    }
#endif
}

void JZThread::pushSender(JZObject *obj)
{
    m_senderQueue.push_back(obj);
}

void JZThread::popSender()
{
    m_senderQueue.pop_back();
}

void JZThread::waitEvents()
{
    auto now = JZPlatform::getTickCountMs();
    int64_t wait_ms = 100;

    JZMutexLocker lock(&m_mutex);
    if (!m_running)
        return;

    for (int i = 0; i < (int)m_timerList.size(); i++)
    {
        int64_t gap = m_timerList[i].nextTrigger - now;
        if (gap < wait_ms)        
            wait_ms = gap;        
    }
    //等待时间,wait_ms < 0 无需等待，直接返回
    if (wait_ms < 0)
        return;

    if (m_eventList.empty())
    {
        m_cv.wait_for(lock, wait_ms,
            [this]()->bool { return !m_running || !m_eventList.empty(); });
    }
}

void JZThread::wakeUp()
{
    m_cv.notify_one();
}

void JZThread::exec()
{
    while (m_running)
    {
        processEvents();
        waitEvents();
    }
}

void JZThread::run()
{    
    exec();
}

int JZThread::addTimer(JZObject *object, int intervalMs)
{    
    JZ_ASSERT(jzApp->currentThread() == this);

    int64_t now = JZPlatform::getTickCountMs();

    TimerInfo info;
    info.object = object;
    info.intervalMs = intervalMs;
    info.id = m_nextTimerId++;
    info.nextTrigger = now + intervalMs;
    m_timerList.push_back(info);
    return info.id;
}

void JZThread::removeTimer(JZObject *object, int timerId)
{
    JZ_ASSERT(jzApp->currentThread() == this);

    int idx = indexOfTimer(object, timerId);
    if (idx >= 0)
    {        
        vec_removeAt(m_timerList, idx);
    }
}

void JZThread::cancelObjectTimers(JZObject *object)
{
    JZ_ASSERT(jzApp->currentThread() == this);
    for (int i = (int)m_timerList.size() - 1; i >= 0; i--)
    {
        if (m_timerList[i].object == object)
            vec_removeAt(m_timerList, i);
    }
}

int JZThread::indexOfTimer(JZObject *object, int timerId)
{
    for (int i = 0; i < (int)m_timerList.size(); i++)
    {
        if (m_timerList[i].id == timerId && m_timerList[i].object == object)
            return i;        
    }
    return -1;
}

void JZThread::postEvent(const JZEventInfo &event)
{
    JZMutexLocker lock(&m_mutex);
    m_eventList.push(event);
    wakeUp();
}

void JZThread::processTimers()
{
    JZTX_FUNC_TRACE

    int64_t start_time = JZPlatform::getTickCountMs();
    while (true)
    {
        TimerInfo time;
        {
            for (int i = 0; i < (int)m_timerList.size(); i++)
            {
                if (start_time >= m_timerList[i].nextTrigger)
                {
                    time = m_timerList[i];
                    break;
                }
            }
        }
        if (time.id == INVAILD_TIMER_ID)
            return;

        JZTimerEvent* timer_event = new JZTimerEvent(time.id);
        JZ_ASSERT(time.object->thread() == this);

        time.object->event(timer_event);

        int idx = indexOfTimer(time.object, time.id);
        if (idx >= 0)
        {
            TimerInfo* ptr = &m_timerList[idx];
            ptr->nextTrigger = start_time + ptr->intervalMs;
        }
    }
}

void JZThread::threadMoveWarning(JZObject *object)
{
    std::string name = object->objectName();
    jzWarning() << "object" << object << "moved after post to thread, current thread:" << this << ", "
        << "object thread:" << object->thread();
}

void JZThread::processEvents()
{    
    JZTX_FUNC_TRACE

    processTimers();

    std::vector<JZEventInfo> eventsToProcess;
    while (true)
    {
        JZEventInfo event;
        {
            JZMutexLocker lock(&m_mutex);
            if (m_eventList.empty())
                return;

            event = m_eventList.front();
            m_eventList.pop();
        }
        if (!event.event)
            return;
        
        if (event.event->type() == JZEvent_asyncSignal)
        {
            JZAsyncSignalEvent* signal_event = (JZAsyncSignalEvent*)event.event;
            if (signal_event->hasRecv)
            {
                JZObject* recv = signal_event->recv.object();
                if (recv)
                {
                    if (recv->thread() != this)
                        threadMoveWarning(recv);

                    JZObject* sender = signal_event->sender;
                    if (sender)
                        m_thread->pushSender(sender);

                    if (signal_event->slot)
                        signal_event->slot(recv, signal_event->args);
                    else
                        signal_event->lambdaSlot(signal_event->args);

                    if (sender)
                        m_thread->popSender();
                }
            }
            else
            {
                JZObject* sender = signal_event->sender;
                if (sender)
                    m_thread->pushSender(sender);

                signal_event->lambdaSlot(signal_event->args);

                if (sender)
                    m_thread->popSender();
            }
        }
        else if (event.event->type() == JZEvent_deleteObject)
        {
            JZObjectDeleteEvent* del_event = (JZObjectDeleteEvent*)event.event;
            auto del_obj = del_event->object.object();
            if(del_obj)
                delete del_obj;
        }
        else
        { 
            //普通消息
            JZObject* object = event.object.object();
            if (object)
            {
                if (object->thread() != this)
                    threadMoveWarning(object);
                
                object->event(event.event);
            }
        }

        delete event.event;
        if (event.sem)
            event.sem->release();
    }
}

void runOnThread(JZThread* thread, std::function<void()> func)
{
    JZAsyncSignalEvent* event = new JZAsyncSignalEvent();
    event->lambdaSlot = jzsignal::make_async_slot_lambda_call(func);
    jzApp->postEvent(thread, event);
}

void runOnThread(JZThread* thread, JZObject* recv, std::function<void()> func)
{
    JZAsyncSignalEvent* event = new JZAsyncSignalEvent();
    event->hasRecv = true;
    event->recv = recv->guardPointer();
    event->lambdaSlot = jzsignal::make_async_slot_lambda_call(func);
    jzApp->postEvent(thread, event);
}

void runOnObject(JZObject* recv, std::function<void()> func)
{
    JZAsyncSignalEvent* event = new JZAsyncSignalEvent();
    event->hasRecv = true;
    event->recv = recv->guardPointer();
    event->lambdaSlot = jzsignal::make_async_slot_lambda_call(func);
    jzApp->postEvent(recv->thread(), event);
}