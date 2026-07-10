#include "JZGpio.h"
#include "JZGpioDriver.h"

JZGpio::JZGpio(JZObject *parent)
    : JZObject(parent), sigLevelChanged(this), sigTriggered(this)
{
}

JZGpio::JZGpio(int pin, JZObject *parent)
    : JZObject(parent), sigLevelChanged(this), sigTriggered(this)
{
    m_pin = pin;
}

JZGpio::~JZGpio()
{
    close();
}

void JZGpio::setPin(int pin)
{
    m_pin = pin;
}

int JZGpio::pin() const
{
    return m_pin;
}

std::vector<JZGpioInfo> JZGpio::availableGpios()
{
    if (JZGpioDriver *drv = JZGpioDriver::instance())
        return drv->availableGpios();
    return {};
}

bool JZGpio::open()
{
    JZGpioDriver *drv = JZGpioDriver::instance();
    if (!drv || m_pin < 0)
        return false;

    m_open = drv->open(this);
    return m_open;
}

void JZGpio::close()
{
    if (!m_open)
        return;

    if (JZGpioDriver *drv = JZGpioDriver::instance())
    {
        drv->stopWaveform(this);
        drv->stopPwm(this);
        drv->close(this);
    }

    m_open = false;
    m_pwmRunning = false;
    m_waveRunning = false;
    m_waveform = WaveNone;
}

bool JZGpio::isOpen() const
{
    return m_open;
}

bool JZGpio::setDirection(Direction dir)
{
    m_direction = dir;
    if (m_open)
    {
        if (JZGpioDriver *drv = JZGpioDriver::instance())
            return drv->setDirection(this, dir);
        return false;
    }
    return true;
}

JZGpio::Direction JZGpio::direction() const
{
    return m_direction;
}

bool JZGpio::setPull(Pull pull)
{
    m_pull = pull;
    if (m_open)
    {
        if (JZGpioDriver *drv = JZGpioDriver::instance())
            return drv->setPull(this, pull);
        return false;
    }
    return true;
}

JZGpio::Pull JZGpio::pull() const
{
    return m_pull;
}

// ==================== 数字 IO ====================
bool JZGpio::write(int level)
{
    if (!m_open)
        return false;
    if (JZGpioDriver *drv = JZGpioDriver::instance())
    {
        if (drv->write(this, level ? High : Low))
        {
            m_level = level ? High : Low;
            return true;
        }
    }
    return false;
}

int JZGpio::read() const
{
    if (!m_open)
        return -1;
    if (JZGpioDriver *drv = JZGpioDriver::instance())
    {
        int v = drv->read(const_cast<JZGpio *>(this));
        if (v >= 0)
            const_cast<JZGpio *>(this)->m_level = v ? High : Low;
        return v;
    }
    return -1;
}

// ==================== PWM ====================
bool JZGpio::startPwm(int freqHz, float dutyCycle)
{
    if (!m_open || freqHz <= 0)
        return false;
    if (dutyCycle < 0.0f) dutyCycle = 0.0f;
    if (dutyCycle > 1.0f) dutyCycle = 1.0f;

    JZGpioDriver *drv = JZGpioDriver::instance();
    if (!drv)
        return false;

    // PWM 与波形互斥
    if (m_waveRunning)
        stopWaveform();

    m_pwmFreq = freqHz;
    m_pwmDuty = dutyCycle;
    m_pwmRunning = drv->startPwm(this, freqHz, dutyCycle);
    return m_pwmRunning;
}

bool JZGpio::setPwmFrequency(int freqHz)
{
    if (freqHz <= 0)
        return false;
    m_pwmFreq = freqHz;
    if (m_pwmRunning)
    {
        if (JZGpioDriver *drv = JZGpioDriver::instance())
            return drv->setPwmFrequency(this, freqHz);
        return false;
    }
    return true;
}

bool JZGpio::setPwmDuty(float dutyCycle)
{
    if (dutyCycle < 0.0f) dutyCycle = 0.0f;
    if (dutyCycle > 1.0f) dutyCycle = 1.0f;
    m_pwmDuty = dutyCycle;
    if (m_pwmRunning)
    {
        if (JZGpioDriver *drv = JZGpioDriver::instance())
            return drv->setPwmDuty(this, dutyCycle);
        return false;
    }
    return true;
}

void JZGpio::stopPwm()
{
    if (!m_pwmRunning)
        return;
    if (JZGpioDriver *drv = JZGpioDriver::instance())
        drv->stopPwm(this);
    m_pwmRunning = false;
}

bool JZGpio::isPwmRunning() const
{
    return m_pwmRunning;
}

int JZGpio::pwmFrequency() const
{
    return m_pwmFreq;
}

float JZGpio::pwmDuty() const
{
    return m_pwmDuty;
}

// ==================== 模拟波形 ====================
bool JZGpio::startWaveform(Waveform type, int freqHz, int amplitude, int offset)
{
    if (!m_open || freqHz <= 0 || type == WaveNone)
        return false;

    JZGpioDriver *drv = JZGpioDriver::instance();
    if (!drv)
        return false;

    // 波形与 PWM 互斥
    if (m_pwmRunning)
        stopPwm();

    m_waveform = type;
    m_waveFreq = freqHz;
    m_waveAmplitude = amplitude;
    m_waveOffset = offset;
    m_waveRunning = drv->startWaveform(this, type, freqHz, amplitude, offset);
    if (!m_waveRunning)
        m_waveform = WaveNone;
    return m_waveRunning;
}

void JZGpio::stopWaveform()
{
    if (!m_waveRunning)
        return;
    if (JZGpioDriver *drv = JZGpioDriver::instance())
        drv->stopWaveform(this);
    m_waveRunning = false;
    m_waveform = WaveNone;
}

bool JZGpio::isWaveformRunning() const
{
    return m_waveRunning;
}

JZGpio::Waveform JZGpio::waveform() const
{
    return m_waveform;
}

// ==================== 触发 ====================
bool JZGpio::setTriggerEdge(Edge edge)
{
    m_triggerEdge = edge;
    if (m_open)
    {
        if (JZGpioDriver *drv = JZGpioDriver::instance())
            return drv->setTriggerEdge(this, edge);
        return false;
    }
    return true;
}

JZGpio::Edge JZGpio::triggerEdge() const
{
    return m_triggerEdge;
}

int JZGpio::level() const
{
    return m_level;
}

// ==================== 监控开关 ====================
void JZGpio::setMonitorEnabled(bool on)
{
    m_monitorEnabled = on;
}

bool JZGpio::monitorEnabled() const
{
    return m_monitorEnabled;
}

// ==================== 驱动回调 ====================
void JZGpio::onLevelChanged(int level)
{
    int newLevel = level ? High : Low;
    int oldLevel = m_level;
    if (newLevel == oldLevel)
        return;

    m_level = newLevel;

    // 仅在启用监控时才下发消息
    if (m_monitorEnabled)
        sigLevelChanged.emit(newLevel);

    // 判定是否满足触发边沿
    bool rising = (oldLevel == Low && newLevel == High);
    bool falling = (oldLevel == High && newLevel == Low);

    bool fire = false;
    switch (m_triggerEdge)
    {
    case RisingEdge:  fire = rising; break;
    case FallingEdge: fire = falling; break;
    case BothEdge:    fire = rising || falling; break;
    case NoEdge:
    default:          fire = false; break;
    }

    if (fire && m_monitorEnabled)
        sigTriggered.emit(newLevel);
}
