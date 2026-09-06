#include "JZFixedTimer.h"
#include "JZDebug.h"

JZFixedTimer::JZFixedTimer(JZObject *parent)
    : JZObject(parent), sigTimeout(this), sigError(this)
{
    m_timeId = INVALID_TIMER_ID;
    m_interval = 0;
    m_maxCatchUp = 5;
    m_nextEmitMs = 0;
    m_frameCount = 0;
}

JZFixedTimer::~JZFixedTimer()
{
    stop();
}

void JZFixedTimer::start(int intervalMs)
{
    start(intervalMs, m_maxCatchUp);
}

void JZFixedTimer::start(int intervalMs, int maxCatchUp)
{
    if (m_timeId != INVALID_TIMER_ID)
    {
        jzWarning() << "JZFixedTimer already started";
        return;
    }
    if (intervalMs <= 0)
    {
        jzWarning() << "JZFixedTimer interval must be > 0";
        return;
    }
    if (maxCatchUp <= 0)
    {
        jzWarning() << "JZFixedTimer maxCatchUp must be > 0";
        return;
    }

    m_interval = intervalMs;
    m_maxCatchUp = maxCatchUp;
    m_frameCount = 0;

    m_elapsed.start();
    m_nextEmitMs = m_interval;   // 第一帧应在启动后一个步长处

    m_timeId = startTimer(m_interval);
}

void JZFixedTimer::stop()
{
    if (m_timeId != INVALID_TIMER_ID)
    {
        killTimer(m_timeId);
        m_timeId = INVALID_TIMER_ID;
    }
}

void JZFixedTimer::restart()
{
    stop();
    start(m_interval, m_maxCatchUp);
}

bool JZFixedTimer::isActive() const
{
    return m_timeId != INVALID_TIMER_ID;
}

int JZFixedTimer::interval() const
{
    return m_interval;
}

void JZFixedTimer::setMaxCatchUp(int frames)
{
    m_maxCatchUp = frames;
}

int JZFixedTimer::maxCatchUp() const
{
    return m_maxCatchUp;
}

int64_t JZFixedTimer::frameCount() const
{
    return m_frameCount;
}

void JZFixedTimer::timerEvent(JZTimerEvent *event)
{
    int64_t now = m_elapsed.elapsed();
    if (now < m_nextEmitMs)
        return;   // 还没到下一帧(底层定时器略早触发), 跳过, 等下一次

    // 根据真实流逝时间计算应补发的帧数
    int64_t behind = now - m_nextEmitMs;
    int frames = (int)(behind / m_interval) + 1;

    if (frames > m_maxCatchUp)
    {
        // 落后太多, 补不过来 -> 停止并上报
        jzWarning() << "JZFixedTimer: too many frames to catch up (" << frames
                    << " > " << m_maxCatchUp << "), stopping";
        stop();
        sigError.emit(frames);
        return;
    }

    for (int i = 0; i < frames; i++)
    {
        sigTimeout.emit();
        m_frameCount++;
        if (!isActive())   // 槽内调用了 stop()
            return;
    }

    // 对齐到墙钟而非简单累加, 避免补帧造成的漂移累积
    m_nextEmitMs += (int64_t)frames * m_interval;
}
