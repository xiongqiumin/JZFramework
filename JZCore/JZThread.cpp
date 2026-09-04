#include "JZThread.h"
#include "JZStd.h"
#include "JZPlatform.h"
#include "JZApplication.h"
#include "JZLog.h"
#include "JZTx.h"
#include "JZDebug.h"

// 平台相关头文件仅在实现文件中包含
#if JZ_OS == JZ_OS_FREE_RTOS
    #include "JZCore/private/JZFreeRTOS.h"
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

//JZThreadConfig
int JZThreadConfig::stackSizeAlign() const
{
    int stack_align = (stackSize + 15) / 16 * 16;
    return stack_align;
}

//JZThread
JZThread::TimerInfo::TimerInfo()
{
    object = nullptr;
    id = INVALID_TIMER_ID;
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
    // 排空未处理事件队列: 释放裸指针 event 与信号量, 避免线程销毁时泄漏
    while (!m_eventList.empty())
    {
        JZEventInfo &event = m_eventList.front();        
        if (event.sem)
            event.sem->release();
        delete event.event;
        m_eventList.pop();
    }    

    //thread 无效了,这里要特殊处理下,先删除
    std::vector<JZObject*> childs = m_childs;
	for (int i = 0; i < (int)childs.size(); i++)
		delete childs[i];
	m_childs.clear();
    
    if(jzApp->mainThread() != this)
        m_thread->cancelObjectTimers(this);

    //后续JZObjet 析构会用到
	if (m_thread == this)
		m_thread = nullptr;
}

void JZThread::initParam()
{
    m_running = false;

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
    jzApp->removeThread(this);    
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
    //加锁保证有tid后才允许
    std::mutex *tmp_mutex = new std::mutex();
    tmp_mutex->lock();
    m_handle = std::thread([this,tmp_mutex]{        
        tmp_mutex->lock();
        tmp_mutex->unlock();
        delete tmp_mutex;

        sigStarted.emit();
        this->run();
        m_running = false;
        sigFinished.emit();
        jzApp->removeThread(this);
    });
    m_tid = m_handle.get_id();
    tmp_mutex->unlock();

#elif JZ_OS == JZ_OS_FREE_RTOS
    // FreeRTOS：创建任务
    m_waitSem = xSemaphoreCreateBinary();

    int pri = 0;
    if(m_config.priority == -1)
        pri = uxTaskPriorityGet(NULL);
    else
    {
        // 1. 先钳位用户输入到 0~15 的合法范围
        int userPri = m_config.priority;
        if (userPri < 0) userPri = 0;
        if (userPri > 15) userPri = 15;

        // 2. 线性映射到 FreeRTOS 合法范围: 0 ~ (configMAX_PRIORITIES - 1)
        UBaseType_t maxSysPri = configMAX_PRIORITIES - 1;
        pri = static_cast<UBaseType_t>((userPri * maxSysPri) / 16);
    }

    int stack_byte_size = m_config.stackSizeAlign();

    // 绑核: cpuCore < 0 表示不绑定(使用 tskNO_AFFINITY); 否则绑定到指定核。
    // 注意 ESP-IDF 的 xTaskCreatePinnedToCore 最后一个参数为 UBaseType_t coreID,
    // tskNO_AFFINITY 表示由调度器自由分配(等价于未绑核行为)。
    UBaseType_t coreAffinity = tskNO_AFFINITY;
    if (m_config.cpuCore >= 0)
        coreAffinity = static_cast<UBaseType_t>(m_config.cpuCore);

    BaseType_t ret = xTaskCreatePinnedToCore(
        // 任务入口函数（静态函数，通过参数传递 this 指针）
        [](void* arg) {
            JZThread* thread = static_cast<JZThread*>(arg);
            thread->sigStarted.emit();                             
            thread->run();            
            thread->m_running = false;
            thread->sigFinished.emit();
            jzApp->removeThread(thread);            

            xSemaphoreGive((SemaphoreHandle_t)thread->m_waitSem);            
            vTaskDelete(NULL);
        },
        objectName().c_str(),
        stack_byte_size / sizeof(StackType_t),
        this,
        pri,
        (TaskHandle_t*)(&m_handle),
        coreAffinity
    );
    JZ_ASSERT(ret == pdPASS);    
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
            thread->sigStarted.emit();
            // 执行任务入口函数
            thread->run();
            // 任务退出：更新状态 + 从 jzApp 移除
            thread->m_running = false;
            thread->sigFinished.emit();
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
    JZ_ASSERT(task != RT_NULL);
    // 启动任务
    m_handle = task;
    rt_thread_startup(task);
    // 保存任务句柄和 ID    
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
    int64_t wait_ms = 1000;

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
        m_cv.wait_for(lock, (int)wait_ms,
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

    int next_time_id = 0;
    for(size_t i = 0; i < m_timerList.size(); i++)
    {
        next_time_id = std::max(next_time_id, m_timerList[i].id);
    }
    next_time_id = next_time_id + 1;

    TimerInfo info;
    info.object = object;
    info.intervalMs = intervalMs;
    info.id = next_time_id++;
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
    // 只拷贝已到期的定时器(避免整表快照), 按 nextTrigger 升序排序后遍历:
    // 1) 一次调用对每个定时器只扫描一次 => 0 间隔也不会死循环;
    // 2) 排序后按时间先后触发, 语义更合理;
    // 3) 快照防止 event() 中增删定时器破坏本轮迭代。
    std::vector<TimerInfo> due_list;
    for (const auto& t : m_timerList)
    {
        if (start_time >= t.nextTrigger)
            due_list.push_back(t);
    }
    std::sort(due_list.begin(), due_list.end(),
              [](const TimerInfo& a, const TimerInfo& b) { return a.nextTrigger < b.nextTrigger; });

    for (const auto& t : due_list)
    {
        // 本轮开始后才被删除的定时器跳过(快照里还保留, 但真实列表已无)
        if (indexOfTimer(t.object, t.id) < 0)
            continue;

        JZTimerEvent timer_event(t.id);
        JZ_ASSERT(t.object->thread() == this);

        notify(t.object, &timer_event);   // 统一入口: 定时器事件同样过过滤器链

        int idx = indexOfTimer(t.object, t.id);
        if (idx >= 0)
        {
            TimerInfo* ptr = &m_timerList[idx];
            if (ptr->intervalMs <= 0)
            {
                // 0 间隔(或负数): 本轮只触发一次(快照单遍扫描保证), 不人为节流。
                // 续约到"当前时刻", 下一轮 exec 立即再次到期 → 频率由事件循环转速决定, 无上限。
                ptr->nextTrigger = JZPlatform::getTickCountMs();
            }
            else
            {
                // 正常间隔: 本轮只触发一次, 续约到 start_time + interval 即可。
                // (t 能进入 due_list 已保证 start_time >= nextTrigger, 无需再 max)
                ptr->nextTrigger = start_time + ptr->intervalMs;
            }
        }
    }
}

void JZThread::threadMoveWarning(JZObject *object)
{
    std::string name = object->objectName();
    jzWarning() << "object" << object << "moved after post to thread, current thread:" << this << ", "
        << "object thread:" << object->thread();
}

void JZThread::notify(JZObject *object, JZEvent *event)
{
    // 事件进入对象的统一咽喉 (sendEvent 同步 / 异步队列 / 定时器都汇聚于此):
    // 先过对象的事件过滤器链, 被拦截则不再派发到 object->event()。
    if (object->applyEventFilters(event))
        return;
    object->event(event);
}

void JZThread::processEvents()
{    
    JZTX_FUNC_TRACE

    processTimers();

    std::vector<JZEventInfo> eventsToProcess;
    int max_event = 0;
    int event_processed = 0;

    {
        //单轮只处理已在队列的数据
        JZMutexLocker lock(&m_mutex);
        max_event = m_eventList.size();
        if (max_event == 0)
            return;
    }

    while (event_processed < max_event)
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

                    signal_event->callback->invoke(recv, signal_event->args);
                    
                    if (sender)
                        m_thread->popSender();
                }
            }
            else
            {
                JZObject* sender = signal_event->sender;
                if (sender)
                    m_thread->pushSender(sender);

                signal_event->callback->invoke(nullptr, signal_event->args);

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

                notify(object, event.event);                
            }
        }
        
        if (event.sem)
            event.sem->release();
        else
            delete event.event;
        
        event_processed++;
    }
}

//JZTaskThread
void JZTaskThread::setTask(const std::function<void()> &task)
{
    m_task = task;
}

void JZTaskThread::run()
{
    m_task();
}

void runOnThread(JZThread* thread, const std::function<void()>& func)
{
    JZAsyncSignalEvent* event = new JZAsyncSignalEvent();

    auto callback = new JZSlotCallback<void>();
    callback->lambdaFunc = func;
    event->callback = callback;

    JZEventInfo e(thread, event);
    thread->postEvent(e);
}

void runOnObject(JZObject* recv, const std::function<void()> &func)
{
    JZAsyncSignalEvent* event = new JZAsyncSignalEvent();
    event->hasRecv = true;
    event->recv = recv->guardPointer();

    auto callback = new JZSlotCallback<void>();
    callback->lambdaFunc = func;
    event->callback = callback;
    jzApp->postEvent(recv->thread(), event);
}

void runOnObject(JZObjectPointer recv, const std::function<void()> &func)
{
    JZObjectPointerGuard guard(&recv);
    auto obj = guard.object();
    if(!obj)
        return;

    runOnObject(obj, func);
}