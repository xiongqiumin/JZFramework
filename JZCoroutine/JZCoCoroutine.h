#ifndef JZCO_CO_COROUTINE_H_
#define JZCO_CO_COROUTINE_H_

#include <functional>
#include <memory>
#include <vector>
#include <stdexcept>
#include "JZCoContext.h"
#include "JZThread.h"

class JZCoScheduler;

class JZCoCoroutineInterrupt : public std::runtime_error 
{
public:
    JZCoCoroutineInterrupt() 
        : std::runtime_error("coroutine interrupted") {}
};

/*
注意：协程依赖主线程调度，主线程不能阻塞
     协程中不要执行事件循环和信号响应，有调度风险
     假设协程co1 调用event loop, 这时触发切换到co2, co2 让出时切换回main, 无法切换回co1 
     协程让出后 -> 总是唤醒主线程 -> 再由主线程调度
*/
class JZCoCoroutine
{
public:
    explicit JZCoCoroutine(int stack_size = 1024);
    virtual ~JZCoCoroutine();

    std::string name();
    void setName(std::string name);

    void setTask(const std::function<void()>& func);
    void run();

    void quit();
    bool isInterrupt() const;
    bool isFinish() const;

protected:
    friend JZCoScheduler;

    virtual void beforeYield();
    virtual void beforeResume();
    virtual void endTask();

    bool m_interrupt;
    bool m_finish;
    std::string m_name;
    JZCoContext* m_ctx;
    std::function<void()> m_func;
};
typedef std::shared_ptr<JZCoCoroutine> JZCoCoroutinePtr;

class JZCoThread : public JZThread 
{
public:
    JZCoThread(std::function<void()> func);

    virtual void run();

protected:
    std::function<void()> m_func;
};

class JZCoScheduler : public JZObject
{
public:
    JZCoScheduler(JZObject *parent = nullptr);
    ~JZCoScheduler();
    
    void init();
    bool isInCoroutine();

    JZCoMainContext* mainContext();
    JZCoCoroutine *currentCoroutine();
    std::vector<JZCoCoroutinePtr> coroutineList();

    void yield(JZCoCoroutine *co);
    void resume(JZCoCoroutine *co);

    void startTask(JZCoCoroutinePtr task);

protected:
    friend void JZCoContextEndTask(JZCoCoroutine* task);

    virtual bool event(JZEvent *event) override;
    void onClearContext();
    void endTask(JZCoCoroutine* task);

    std::vector<JZCoCoroutinePtr> m_coList;
    std::vector<JZCoContext*> m_finished;
    JZCoMainContext* m_mainCtx;
    JZCoCoroutine* m_co;
};
extern JZCoScheduler* g_scheduler;

//JZCoCondition
class JZCoCondition
{
public:
    JZCoCondition();

    void wait(int timeout = -1);
    void wakeOne();
    void wakeAll();

protected:
    JZ_DISABLE_COPY(JZCoCondition)

    std::vector<JZCoCoroutine*> m_coList;
};

#endif // !JZCO_NET_WORK_H
