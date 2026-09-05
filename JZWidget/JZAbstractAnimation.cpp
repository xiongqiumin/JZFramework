#include "JZAbstractAnimation.h"
#include "JZWidget.h"
#include "JZEvent.h"
#include "JZPlatform.h"
#include <cmath>

JZ_OBJECT_IMPL(JZAbstractAnimation, JZObject)

// 缓动曲线 (输入 t∈[0,1], 输出也是 [0,1] 的进度曲线)
static double jzEaseCurve(JZEasing e, double t)
{
    switch (e)
    {
    case JZEasing::EaseIn:
        return t * t * t;
    case JZEasing::EaseOut:
    {
        double u = 1.0 - t;
        return 1.0 - u * u * u;
    }
    case JZEasing::EaseInOut:
        return t < 0.5 ? 4.0 * t * t * t
                       : 1.0 - std::pow(-2.0 * t + 2.0, 3.0) / 2.0;
    case JZEasing::Overshoot:
    {
        // easeOutBack
        const double c1 = 1.70158, c3 = c1 + 1.0;
        double u = t - 1.0;
        return 1.0 + c3 * u * u * u + c1 * u * u;
    }
    case JZEasing::Bounce:
        // easeOutBounce
        if (t < 1.0 / 2.75)
            return 7.5625 * t * t;
        if (t < 2.0 / 2.75)
        {
            t -= 1.5 / 2.75;
            return 7.5625 * t * t + 0.75;
        }
        if (t < 2.5 / 2.75)
        {
            t -= 2.25 / 2.75;
            return 7.5625 * t * t + 0.9375;
        }
        t -= 2.625 / 2.75;
        return 7.5625 * t * t + 0.984375;
    case JZEasing::Linear:
    default:
        return t;
    }
}

JZAbstractAnimation::JZAbstractAnimation(JZObject* parent)
    : JZObject(parent)
    , sigFinished(this)
{
}

JZAbstractAnimation::~JZAbstractAnimation()
{
    stop();
}

void JZAbstractAnimation::setTarget(void* obj)
{
    m_target = obj;
    m_targetIsWidget = false;
}

void JZAbstractAnimation::setTarget(JZWidget* widget)
{
    m_target = widget;
    m_targetIsWidget = true;
}

void JZAbstractAnimation::setDuration(uint32_t ms)
{
    m_duration = ms;
}

void JZAbstractAnimation::setDelay(uint32_t ms)
{
    m_delay = ms;
}

void JZAbstractAnimation::setEasingLinear()      { m_easing = JZEasing::Linear; }
void JZAbstractAnimation::setEasingEaseIn()      { m_easing = JZEasing::EaseIn; }
void JZAbstractAnimation::setEasingEaseOut()     { m_easing = JZEasing::EaseOut; }
void JZAbstractAnimation::setEasingEaseInOut()   { m_easing = JZEasing::EaseInOut; }
void JZAbstractAnimation::setEasingOvershoot()   { m_easing = JZEasing::Overshoot; }
void JZAbstractAnimation::setEasingBounce()      { m_easing = JZEasing::Bounce; }

void JZAbstractAnimation::setExecCb(JZAnimExecCb cb)
{
    m_execCb = cb;
}

void JZAbstractAnimation::setStartValue(int32_t v)
{
    m_from = v;
}

void JZAbstractAnimation::setEndValue(int32_t v)
{
    m_to = v;
}

void JZAbstractAnimation::setRange(int32_t from, int32_t to)
{
    m_from = from;
    m_to = to;
}

void JZAbstractAnimation::onValueChanged(int32_t value)
{
    if (m_execCb)
        m_execCb(m_target, value);
}

void JZAbstractAnimation::applyValue(int32_t v)
{
    m_curValue = v;
    onValueChanged(v);
}

void JZAbstractAnimation::start()
{
    if (m_running)
        stop();

    m_beginMs = JZPlatform::getTickCountMs() + m_delay;
    m_running = true;

    if (m_duration == 0)
    {
        finish();
        return;
    }

    applyValue(m_from);                 // 先落到起始值 (无跳变)
    m_timerId = startTimer(16);         // 帧驱动: 真实粒度由线程定时器决定 (~30ms)
}

void JZAbstractAnimation::stop()
{
    if (!m_running && m_timerId < 0)
        return;
    m_running = false;
    if (m_timerId > 0)
    {
        killTimer(m_timerId);
        m_timerId = -1;
    }
}

void JZAbstractAnimation::tick(int64_t nowMs)
{
    if (!m_running)
        return;

    int64_t elapsed = nowMs - m_beginMs;
    if (elapsed < 0)
        return;                                 // 仍在延迟期

    if (elapsed >= (int64_t)m_duration)
    {
        finish();                               // 收尾落在精确终点值
        return;
    }

    double p = jzEaseCurve(m_easing, (double)elapsed / (double)m_duration);
    int64_t v = m_from + (int64_t)llround((double)(m_to - m_from) * p);
    applyValue((int32_t)v);
}

void JZAbstractAnimation::finish()
{
    m_running = false;
    if (m_timerId > 0)
    {
        killTimer(m_timerId);
        m_timerId = -1;
    }
    applyValue(m_to);
    sigFinished.emit();
}

void JZAbstractAnimation::timerEvent(JZTimerEvent* event)
{
    if (event->id() != m_timerId)
    {
        JZObject::timerEvent(event);
        return;
    }
    tick(JZPlatform::getTickCountMs());
}
