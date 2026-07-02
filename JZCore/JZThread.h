#ifndef JZ_THREAD_H_
#define JZ_THREAD_H_

#include <queue>
#include "JZObject.h"
#include "JZEventLoop.h"
#include "JZConditionVariable.h"

#ifdef JZ_OS_NORMAL
#include <thread>

typedef std::thread::id JZThreadId;
#define THREAD_ID_NONE  std::thread::id()

#else

typedef void* JZThreadId;
#define THREAD_ID_NONE  nullptr
#endif

class JZEventInfo
{
public:
    JZEventInfo();
    JZEventInfo(JZObject *obj, JZEvent *e);

    JZObjectPointer object;
    JZEvent *event;
    JZSemaphore *sem;
};

class JZThreadConfig
{
public:
    int stackSizeAlign() const;

    int stackSize = 4096;
    int priority = -1; // 0-15, 15最高, -1 继承当前
};

class JZApplication;
class JZThread : public JZObject
{
public:    
    static void msleep(int ms);
    static JZThread *currentThread();
    static JZThreadId currentThreadId();

    JZThread(JZObject *parent = nullptr);
    virtual ~JZThread();
     
    void setConfig(JZThreadConfig config);
    JZThreadConfig config() const;

    bool isRunning() const;

    JZThreadId id();
    void attachStdThread(JZThreadId id);        
    void detachStdThread();

    void start();
    void quit();
    void wait();    

    bool isRequestInterruption() const;
    void requestInterruption();

jz_signals:
    JZSigVoid sigStarted;
    JZSigVoid sigFinished;

protected:
    friend JZObject;
    friend JZApplication;
    friend JZEventLoop;
    friend JZBaseSignal;

    struct TimerInfo 
    {
        TimerInfo();

        int id;
        JZObject *object;        
        int intervalMs;
        int64_t nextTrigger;
    };

    JZThread(JZObject::MainThreadInit);
    void initParam();

    virtual void run();

    int addTimer(JZObject *object, int intervalMs);
    void removeTimer(JZObject *object, int timerId);
    void cancelObjectTimers(JZObject *object);

    void pushSender(JZObject *obj);
    void popSender();

    void waitEvents();
    void wakeUp();
    void exec();

    void postEvent(const JZEventInfo &event);
    void processTimers();
    void processEvents();
    int indexOfTimer(JZObject *object, int timerId);
    void threadMoveWarning(JZObject* object);

    bool m_running;    
#ifdef JZ_OS_NORMAL    
    std::thread::id m_tid; //为了attach 必须和 m_handle 分开
    std::thread m_handle;
#elif JZ_OS == JZ_OS_FREE_RTOS
    void *m_waitSem;
    void *m_handle;
#elif JZ_OS == JZ_OS_RT_THREAD
    void *m_handle;
#else
    void *m_handle;
#endif

    JZThreadConfig m_config;
    std::vector<TimerInfo> m_timerList;
    std::queue<JZEventInfo> m_eventList;
    std::vector<JZObject*> m_senderQueue;   // 信号发送栈 (从 JZObject 移入)
    JZConditionVariable m_cv;
    JZMutex m_mutex;
    int m_nextTimerId;
};

// 在thread所在线程执行
void runOnThread(JZThread *thread, std::function<void()> func);
// 在thread所在线程执行, recv 删除后不会执行
void runOnThread(JZThread *thread, JZObject *recv, std::function<void()> func);
// 在recv所在线程执行, recv 删除后不会执行
void runOnObject(JZObject *recv, std::function<void()> func);

#endif // !JZ_THREAD_H_
