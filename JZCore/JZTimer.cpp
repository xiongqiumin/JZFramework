#include "JZTimer.h"
#include "JZTx.h"
#include "JZDebug.h"

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

//JZTimer
void JZTimer::singleShot(int msec, std::function<void()> func)
{
    JZSingleTimer *t = new JZSingleTimer(msec, nullptr);
    t->func = func;
}

void JZTimer::singleShot(int msec, JZObject *object, std::function<void()> func)
{
    JZSingleTimer *t = new JZSingleTimer(msec, object);
    t->func = func;
}

JZTimer::JZTimer(JZObject *parent)
    :JZObject(parent), sigTimeout(this)
{
    m_timeId = INVALID_TIMER_ID;
    m_singleShot = false;
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