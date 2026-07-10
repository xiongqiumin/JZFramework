#ifndef JZ_GPIO_H_
#define JZ_GPIO_H_

#include "JZObject.h"

// JZGpio 是面向用户的 GPIO 引脚类。它自身不做平台相关操作，
// 所有底层动作都委派给单例 JZGpioDriver（如 JZGpioDriverEsp32 / JZGpioDriverSimulator）。
//
// 功能:
//   - 数字输入/输出 (read / write)
//   - 上下拉、方向配置
//   - PWM 输出 (频率 + 占空比)
//   - 常见模拟波形输出 (方波/正弦/三角/锯齿/脉冲)
//   - 边沿触发 (上升/下降/双边沿) -> sigTriggered
//
// 输入电平变化由驱动通过 onLevelChanged() 异步推入，
// 由本类判断边沿后发出 sigLevelChanged / sigTriggered。
class JZGpio : public JZObject
{
public:
    // 引脚方向
    enum Direction {
        Input = 0,
        Output = 1,
    };

    // 上下拉
    enum Pull {
        PullNone = 0,
        PullUp = 1,
        PullDown = 2,
    };

    // 电平
    enum Level {
        Low = 0,
        High = 1,
    };

    // 触发边沿
    enum Edge {
        NoEdge = 0,
        RisingEdge = 1,   // 上升沿
        FallingEdge = 2,  // 下降沿
        BothEdge = 3,     // 双边沿
    };

    // 波形类型 (用于模拟输出 / DAC / PWM 调制)
    enum Waveform {
        WaveNone = 0,
        WaveSquare = 1,    // 方波
        WaveSine = 2,      // 正弦波
        WaveTriangle = 3,  // 三角波
        WaveSawtooth = 4,  // 锯齿波
        WavePulse = 5,     // 窄脉冲
    };

    explicit JZGpio(JZObject *parent = nullptr);
    explicit JZGpio(int pin, JZObject *parent = nullptr);
    virtual ~JZGpio();

    void setPin(int pin);
    int  pin() const;

    // 枚举当前平台可用 GPIO 引脚(委派给驱动单例)
    static std::vector<JZGpioInfo> availableGpios();

    // 打开(应用当前配置到硬件) / 关闭(释放引脚)
    bool open();
    void close();
    bool isOpen() const;

    bool setDirection(Direction dir);
    Direction direction() const;

    bool setPull(Pull pull);
    Pull pull() const;

    // ============ 数字 IO ============
    // 输出电平 (仅 Output 方向有效)
    bool write(int level);
    // 读取电平 (返回 High/Low, 失败返回 -1)
    int  read() const;

    // ============ PWM ============
    // dutyCycle 取值 0.0 ~ 1.0
    bool startPwm(int freqHz, float dutyCycle);
    bool setPwmFrequency(int freqHz);
    bool setPwmDuty(float dutyCycle);
    void stopPwm();
    bool isPwmRunning() const;
    int   pwmFrequency() const;
    float pwmDuty() const;

    // ============ 模拟波形 ============
    // amplitude: 峰值(平台自行按 DAC/PWM 分辨率映射), offset: 直流偏置
    bool startWaveform(Waveform type, int freqHz, int amplitude = 4095, int offset = 0);
    void stopWaveform();
    bool isWaveformRunning() const;
    Waveform waveform() const;

    // ============ 触发 ============
    bool setTriggerEdge(Edge edge);
    Edge triggerEdge() const;

    // ============ 监控开关 ============
    // 启用后, 电平变化才会下发 sigLevelChanged / sigTriggered。
    // 默认关闭, 避免无关注时空转。
    void setMonitorEnabled(bool on);
    bool monitorEnabled() const;

    // 当前电平缓存(最近一次 read 或 onLevelChanged 的值)
    int level() const;

    // 由底层驱动异步调用: 上报引脚电平变化。
    // 本类据此更新缓存、发出 sigLevelChanged, 并按 triggerEdge 判定 sigTriggered。
    void onLevelChanged(int level);

jz_signals:
    // 电平发生变化(参数为新电平 High/Low)。
    // 仅在 monitorEnabled() 为真时才下发, 见 onLevelChanged。
    JZSigInt  sigLevelChanged;
    // 满足 triggerEdge 条件时触发(参数为触发后的新电平 High/Low,
    // 据此可判断是上升沿 High 还是下降沿 Low)。
    // 仅在 monitorEnabled() 为真时才下发。
    JZSigInt  sigTriggered;

protected:
    JZ_DISABLE_COPY(JZGpio)

    int        m_pin = -1;
    bool       m_open = false;
    Direction  m_direction = Input;
    Pull       m_pull = PullNone;
    Edge       m_triggerEdge = NoEdge;
    int        m_level = Low;

    // 监控开关: 启用后电平变化才下发信号
    bool       m_monitorEnabled = false;

    // PWM 状态
    bool       m_pwmRunning = false;
    int        m_pwmFreq = 1000;
    float      m_pwmDuty = 0.5f;

    // 波形状态
    bool       m_waveRunning = false;
    Waveform   m_waveform = WaveNone;
    int        m_waveFreq = 1000;
    int        m_waveAmplitude = 4095;
    int        m_waveOffset = 0;
};

#endif // !JZ_GPIO_H_
