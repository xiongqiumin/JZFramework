#include "JZGpioDriverSimulator.h"
#include "JZGpioSimPack.h"
#include "JZSignal.h"
#include "JZApplication.h"
#include <cmath>

namespace
{
constexpr double JZ_PI = 3.14159265358979323846;
} // namespace

// ==================== 引脚枚举 ====================
std::vector<JZGpioInfo> JZGpioDriverSimulator::availableGpios()
{
    // 模拟设备暴露的引脚集合(可由设备端配置, 这里给默认值 GPIO0~GPIO27)
    std::vector<JZGpioInfo> list;
    for (int p = 0; p <= 27; p++)
    {
        JZGpioInfo info;
        info.pin = p;
        info.name = "GPIO" + std::to_string(p);
        info.description = "Simulated GPIO";
        list.push_back(info);
    }
    return list;
}

JZGpioDriverSimulator::JZGpioDriverSimulator(const std::string &ip, int port)
{
    JZGpioSimPack::ensureRegistered();

    m_client = new JZNetClient();
    connect(&m_client->sigPacketReceive, [this](JZNetPackPtr p) { onPacket(p); });
    m_client->connectToHost(ip, port);
    if (jzApp)
        m_client->waitForConnected(500);
}

JZGpioDriverSimulator::~JZGpioDriverSimulator()
{
    if (m_waveTimer)
    {
        m_waveTimer->stop();
        delete m_waveTimer;
        m_waveTimer = nullptr;
    }
    if (m_client)
    {
        delete m_client;
        m_client = nullptr;
    }
}

void JZGpioDriverSimulator::sendSim(int msg, const JZVariantMap &m)
{
    if (!m_client || !m_client->isConnect())
        return;
    JZGpioSimPack pack;
    pack.map = m;
    simSetInt(pack.map, "msg", msg);
    m_client->sendPacket(&pack);
}

// ==================== 打开 / 关闭 ====================
bool JZGpioDriverSimulator::open(JZGpio *gpio)
{
    int pin = gpio->pin();
    m_pins[pin] = gpio;
    m_levelCache[pin] = JZGpio::Low;

    JZVariantMap m;
    simSetInt(m, "pin", pin);
    simSetInt(m, "dir", gpio->direction());
    simSetInt(m, "pull", gpio->pull());
    sendSim(SIM_GPIO_CMD_OPEN, m);

    // open 前已设置触发边沿则同步给设备
    if (gpio->direction() == JZGpio::Input && gpio->triggerEdge() != JZGpio::NoEdge)
        setTriggerEdge(gpio, gpio->triggerEdge());
    return true;
}

void JZGpioDriverSimulator::close(JZGpio *gpio)
{
    int pin = gpio->pin();
    JZVariantMap m;
    simSetInt(m, "pin", pin);
    sendSim(SIM_GPIO_CMD_CLOSE, m);
    m_pins.erase(pin);
    m_levelCache.erase(pin);
}

bool JZGpioDriverSimulator::isOpen(JZGpio *gpio)
{
    return m_pins.find(gpio->pin()) != m_pins.end();
}

// ==================== 配置 ====================
bool JZGpioDriverSimulator::setDirection(JZGpio *gpio, JZGpio::Direction dir)
{
    JZVariantMap m;
    simSetInt(m, "pin", gpio->pin());
    simSetInt(m, "dir", dir);
    sendSim(SIM_GPIO_CMD_SET_DIR, m);
    return true;
}

bool JZGpioDriverSimulator::setPull(JZGpio *gpio, JZGpio::Pull pull)
{
    JZVariantMap m;
    simSetInt(m, "pin", gpio->pin());
    simSetInt(m, "pull", pull);
    sendSim(SIM_GPIO_CMD_SET_PULL, m);
    return true;
}

// ==================== 数字 IO ====================
bool JZGpioDriverSimulator::write(JZGpio *gpio, int level)
{
    int pin = gpio->pin();
    m_levelCache[pin] = level ? JZGpio::High : JZGpio::Low;
    JZVariantMap m;
    simSetInt(m, "pin", pin);
    simSetInt(m, "level", level ? 1 : 0);
    sendSim(SIM_GPIO_CMD_WRITE, m);
    return true;
}

int JZGpioDriverSimulator::read(JZGpio *gpio)
{
    // 同步返回本地缓存(由设备通过 SIM_GPIO_EVT_LEVEL 持续刷新)
    auto it = m_levelCache.find(gpio->pin());
    int lv = (it != m_levelCache.end()) ? it->second : JZGpio::Low;

    // 同时向设备请求一次刷新
    JZVariantMap m;
    simSetInt(m, "pin", gpio->pin());
    sendSim(SIM_GPIO_CMD_READ, m);
    return lv;
}

// ==================== PWM ====================
bool JZGpioDriverSimulator::startPwm(JZGpio *gpio, int freqHz, float duty)
{
    JZVariantMap m;
    simSetInt(m, "pin", gpio->pin());
    simSetInt(m, "freq", freqHz);
    simSetInt(m, "duty", (int)(duty * 1000.0f + 0.5f)); // 千分比
    sendSim(SIM_GPIO_CMD_PWM_START, m);
    return true;
}

bool JZGpioDriverSimulator::setPwmFrequency(JZGpio *gpio, int freqHz)
{
    JZVariantMap m;
    simSetInt(m, "pin", gpio->pin());
    simSetInt(m, "freq", freqHz);
    sendSim(SIM_GPIO_CMD_PWM_FREQ, m);
    return true;
}

bool JZGpioDriverSimulator::setPwmDuty(JZGpio *gpio, float duty)
{
    JZVariantMap m;
    simSetInt(m, "pin", gpio->pin());
    simSetInt(m, "duty", (int)(duty * 1000.0f + 0.5f));
    sendSim(SIM_GPIO_CMD_PWM_DUTY, m);
    return true;
}

void JZGpioDriverSimulator::stopPwm(JZGpio *gpio)
{
    JZVariantMap m;
    simSetInt(m, "pin", gpio->pin());
    sendSim(SIM_GPIO_CMD_PWM_STOP, m);
}

// ==================== 波形 (本地 JZTimer 生成) ====================
bool JZGpioDriverSimulator::startWaveform(JZGpio *gpio, JZGpio::Waveform type, int freqHz, int amplitude, int offset)
{
    int pin = gpio->pin();
    WaveState ws;
    ws.type = type;
    ws.freq = freqHz;
    ws.amp = amplitude;
    ws.offset = offset;
    ws.phase = 0.0;
    m_wavePins[pin] = ws;

    if (!m_waveTimer)
    {
        m_waveTimer = new JZTimer();
        connect(&m_waveTimer->sigTimeout, [this]() { onWaveTimeout(); });
    }
    updateWaveTimer();
    return true;
}

void JZGpioDriverSimulator::stopWaveform(JZGpio *gpio)
{
    m_wavePins.erase(gpio->pin());
    updateWaveTimer();
}

void JZGpioDriverSimulator::updateWaveTimer()
{
    if (m_wavePins.empty())
    {
        if (m_waveTimer)
            m_waveTimer->stop();
        return;
    }

    // 取所有活动波形中要求的最小采样周期(每周期约 32 点), 限幅到 [20, 50] ms
    // 下限 20ms 已足够模拟场景, 同时避免定时器精度过高带来的开销
    int minMs = 50;
    for (auto &kv : m_wavePins)
    {
        int ms = (int)(1000.0 / (kv.second.freq * 32.0) + 0.5);
        if (ms < 20) ms = 20;
        if (ms > 50) ms = 50;
        if (ms < minMs)
            minMs = ms;
    }
    m_waveTimerMs = minMs;
    m_waveTimer->start(minMs);
}

void JZGpioDriverSimulator::onWaveTimeout()
{
    // 真实流逝时间(秒) = 定时器周期
    const double dt = m_waveTimerMs / 1000.0;

    for (auto &kv : m_wavePins)
    {
        int pin = kv.first;
        WaveState &ws = kv.second;

        ws.phase += ws.freq * dt;
        while (ws.phase >= 1.0)
            ws.phase -= 1.0;
        double ph = ws.phase;  // 0~1

        // 归一化采样 0~1
        double s = 0.0;
        switch (ws.type)
        {
        case JZGpio::WaveSquare:   s = (ph < 0.5) ? 1.0 : 0.0; break;
        case JZGpio::WaveSine:     s = 0.5 * (1.0 + sin(2.0 * JZ_PI * ph)); break;
        case JZGpio::WaveTriangle: s = (ph < 0.5) ? (ph * 2.0) : (2.0 - ph * 2.0); break;
        case JZGpio::WaveSawtooth: s = ph; break;
        case JZGpio::WavePulse:    s = (ph < 0.05) ? 1.0 : 0.0; break;
        default:                   s = 0.0; break;
        }

        // GPIO 为数字引脚: 以 0.5 为阈值比较器式输出(模拟信号经施密特触发后的数字结果)
        int level = (s >= 0.5) ? JZGpio::High : JZGpio::Low;
        m_levelCache[pin] = level;

        auto it = m_pins.find(pin);
        if (it != m_pins.end() && it->second)
            it->second->onLevelChanged(level);
    }
}

// ==================== 触发 ====================
bool JZGpioDriverSimulator::setTriggerEdge(JZGpio *gpio, JZGpio::Edge edge)
{
    JZVariantMap m;
    simSetInt(m, "pin", gpio->pin());
    simSetInt(m, "edge", edge);
    sendSim(SIM_GPIO_CMD_SET_TRIGGER, m);
    return true;
}

// ==================== 报文分发 ====================
void JZGpioDriverSimulator::onPacket(JZNetPackPtr pack)
{
    auto *sp = dynamic_cast<JZGpioSimPack *>(pack.get());
    if (!sp)
        return;

    const JZVariantMap &map = sp->map;
    int msg = sp->msg();

    switch (msg)
    {
    case SIM_GPIO_EVT_LEVEL:
    case SIM_GPIO_EVT_READ_RESULT:
    {
        int pin = simGetInt(map, "pin");
        int level = simGetInt(map, "level");
        m_levelCache[pin] = level ? JZGpio::High : JZGpio::Low;
        auto it = m_pins.find(pin);
        if (it != m_pins.end() && it->second)
            it->second->onLevelChanged(level);
        break;
    }
    default:
        break;
    }
}
