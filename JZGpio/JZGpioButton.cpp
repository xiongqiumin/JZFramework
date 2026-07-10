#include "JZGpioButton.h"
#include "JZPlatform.h"
#include "JZTimer.h"

JZGpioButton::JZGpioButton(int pin, JZObject *parent)
    : JZObject(parent),
      sigPressed(this), sigReleased(this),
      sigClicked(this), sigDoubleClicked(this), sigLongPressed(this)
{
    // 内部创建并托管 JZGpio(以本对象为父, 析构时自动释放)
    JZGpio *gpio = new JZGpio(pin, this);
    setupGpio(gpio);
}

JZGpioButton::JZGpioButton(const JZGpioInfo &info, JZObject *parent)
    : JZObject(parent),
      sigPressed(this), sigReleased(this),
      sigClicked(this), sigDoubleClicked(this), sigLongPressed(this)
{
    // 内部创建并托管 JZGpio(以本对象为父, 析构时自动释放)
    JZGpio *gpio = new JZGpio(info.pin, this);
    setupGpio(gpio);
}

JZGpioButton::~JZGpioButton()
{
    if (m_timer)
    {
        m_timer->stop();
        delete m_timer;
        m_timer = nullptr;
    }
}

void JZGpioButton::setupGpio(JZGpio *gpio)
{
    m_gpio = gpio;
    m_timer = new JZTimer(this);
    m_timer->setSingleShot(true);
    connect(&m_timer->sigTimeout, [this]() { onTimer(); });

    if (m_gpio)
    {
        // 按键必为输入, 并开启电平监控(供本类识别边沿)
        m_gpio->setDirection(JZGpio::Input);
        m_gpio->setMonitorEnabled(true);
        connect(&m_gpio->sigLevelChanged, this, &JZGpioButton::onLevelChanged);
        // 内部创建或外部未打开时, 自动打开引脚以驱动电平监控
        if (!m_gpio->isOpen())
            m_gpio->open();
    }
}

void JZGpioButton::setActiveLevel(int level)
{
    m_activeLevel = level ? JZGpio::High : JZGpio::Low;
}

int JZGpioButton::activeLevel() const
{
    return m_activeLevel;
}

void JZGpioButton::setPull(JZGpio::Pull pull)
{
    if (m_gpio)
        m_gpio->setPull(pull);
}

JZGpio::Pull JZGpioButton::pull() const
{
    return m_gpio ? m_gpio->pull() : JZGpio::PullNone;
}

void JZGpioButton::setDebounceMs(int ms)
{
    m_debounceMs = ms > 0 ? ms : 0;
}

int JZGpioButton::debounceMs() const
{
    return m_debounceMs;
}

void JZGpioButton::setLongPressMs(int ms)
{
    m_longPressMs = ms > 0 ? ms : 0;
}

int JZGpioButton::longPressMs() const
{
    return m_longPressMs;
}

void JZGpioButton::setDoubleClickMs(int ms)
{
    m_doubleClickMs = ms > 0 ? ms : 0;
}

int JZGpioButton::doubleClickMs() const
{
    return m_doubleClickMs;
}

bool JZGpioButton::isPressed() const
{
    return m_pressed;
}

void JZGpioButton::onLevelChanged(int level)
{
    int64_t now = JZPlatform::getTickCountMs();
    bool cur = (level == m_activeLevel);  // true = 当前为"按下"

    // 消抖: 距上次有效翻转不足 debounceMs 的抖动直接忽略
    if (now - m_lastStableMs < m_debounceMs)
        return;
    m_lastStableMs = now;

    if (cur && !m_pressed)
    {
        // ---- 按下边沿 ----
        m_pressed = true;
        m_pressStartMs = now;
        m_longFired = false;
        sigPressed.emit();

        m_timerMode = TimerLongPress;
        m_timer->start(m_longPressMs);
    }
    else if (!cur && m_pressed)
    {
        // ---- 释放边沿 ----
        m_pressed = false;
        if (m_timerMode == TimerLongPress)
            m_timer->stop();

        int64_t pressMs = now - m_pressStartMs;
        sigReleased.emit();

        if (m_longFired)
        {
            // 已判定为长按, 不再产生单击
            m_waitDouble = false;
            m_timerMode = TimerNone;
        }
        else
        {
            // 短按: 判断是否构成双击
            if (m_waitDouble && (now - m_lastReleaseMs <= m_doubleClickMs))
            {
                if (m_timerMode == TimerClickWait)
                    m_timer->stop();
                sigDoubleClicked.emit();
                m_waitDouble = false;
                m_timerMode = TimerNone;
            }
            else
            {
                // 进入"等待第二击"状态, 超时则判定为单击
                m_waitDouble = true;
                m_lastReleaseMs = now;
                m_timerMode = TimerClickWait;
                m_timer->start(m_doubleClickMs);
            }
        }
    }
}

void JZGpioButton::onTimer()
{
    switch (m_timerMode)
    {
    case TimerLongPress:
        m_longFired = true;
        sigLongPressed.emit();
        m_timerMode = TimerNone;
        break;
    case TimerClickWait:
        sigClicked.emit();
        m_waitDouble = false;
        m_timerMode = TimerNone;
        break;
    default:
        break;
    }
}
