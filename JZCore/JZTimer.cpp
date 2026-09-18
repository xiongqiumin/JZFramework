#include "JZTimer.h"
#include "JZTx.h"
#include "JZDebug.h"
#include "JZThread.h"
#include "JZApplication.h"

class JZSingleTimer : public JZObject
{
public:
    JZSingleTimer(int ms,JZObject *object)
    {
        if (object)
        {
            hasObject = true;
            objectPtr = object->guardPointer();
        }
        else
            hasObject = false;

        m_timerId = startTimer(ms);
    }

    virtual void timerEvent(JZTimerEvent *event)
    {
        killTimer(m_timerId);
        if (!hasObject || (hasObject && objectPtr))
            func();

        deleteLater();        
    }

    std::function<void()> func;    
    int m_timerId;

    bool hasObject;
    JZObjectPointer objectPtr;
};

void JZTimer::singleShot(int msec, std::function<void()> func)
{
    if (msec <= 0)
    {
        runOnThread(jzApp->currentThread(), std::move(func));
        return;
    }

    JZSingleTimer *t = new JZSingleTimer(msec, nullptr);
    t->func = std::move(func);
}

void JZTimer::singleShot(int msec, JZObject *object, std::function<void()> func)
{
    if (msec <= 0)
    {
        // 语义与 JZSingleTimer 保持一致: 在**当前线程**跑; 接收者已销毁就不跑(原来由 objectPtr 判断)
        JZObjectPointer guard;
        if (object)
            guard = object->guardPointer();

        runOnThread(jzApp->currentThread(), [guard, func]() {
            if (guard && !guard.object())
                return;     // 接收者已经不在了: 跳过
            func();
        });
        return;
    }

    JZSingleTimer *t = new JZSingleTimer(msec, object);
    t->func = std::move(func);
}

JZTimer::JZTimer(JZObject *parent)
    :JZObject(parent), sigTimeout(this)
{
    m_timeId = INVALID_TIMER_ID;
    m_singleShot = false;
    m_gap = 0;
}

JZTimer::~JZTimer()
{
    stop();
}

void JZTimer::timerEvent(JZTimerEvent *event)
{
    JZTX_FUNC_TRACE
    if (m_singleShot)
        stop();

    sigTimeout.emit();
}

void JZTimer::start(int ms)
{
    if (m_timeId != INVALID_TIMER_ID)
    {
        jzWarning() << "timer allready stated";
        return;
    }
    
    m_gap = ms;
    m_timeId = startTimer(ms);
}

void JZTimer::stop()
{
    if (m_timeId != INVALID_TIMER_ID)
    {
        killTimer(m_timeId);
        m_timeId = INVALID_TIMER_ID;
    }
}

void JZTimer::restart()
{
    stop();
    startTimer(m_gap);
}

bool JZTimer::isActive() const
{
    return m_timeId != INVALID_TIMER_ID;
}

bool JZTimer::isSingleShot() const
{
    return m_singleShot;
}

void JZTimer::setSingleShot(bool singleShot)
{
    m_singleShot = singleShot;
}