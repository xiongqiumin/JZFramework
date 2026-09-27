#ifndef JZ_GPIO_DRIVER_ESP32_H_
#define JZ_GPIO_DRIVER_ESP32_H_
#include "JZGpioDriver.h"
#include <map>

// 基于 ESP-IDF 的 JZGpioDriver 实现。
//   - 数字 IO : gpio_config / gpio_set_level / gpio_get_level
//   - 触发    : GPIO 中断 (gpio_install_isr_service) + FreeRTOS 队列转发到应用线程,
//               再回调 JZGpio::onLevelChanged()
//   - PWM     : LEDC 外设 (每个引脚分配一个 LEDC 通道)
//   - 波形    : 独立的 FreeRTOS 任务 (waveTask) 周期采样, 输出到 DAC(GPIO25/26) 或调制 LEDC 占空比
class JZFRAMEWORK_API JZGpioDriverEsp32 : public JZGpioDriver
{
public:
    JZGpioDriverEsp32();
    ~JZGpioDriverEsp32() override;

    bool supportsPin(int pin) override;
    std::vector<JZGpioInfo> availableGpios() override;

    bool open(JZGpio *gpio) override;
    void close(JZGpio *gpio) override;
    bool isOpen(JZGpio *gpio) override;

    bool setDirection(JZGpio *gpio, JZGpio::Direction dir) override;
    bool setPull(JZGpio *gpio, JZGpio::Pull pull) override;

    bool write(JZGpio *gpio, int level) override;
    int  read(JZGpio *gpio) override;

    bool startPwm(JZGpio *gpio, int freqHz, float duty) override;
    bool setPwmFrequency(JZGpio *gpio, int freqHz) override;
    bool setPwmDuty(JZGpio *gpio, float duty) override;
    void stopPwm(JZGpio *gpio) override;

    bool startWaveform(JZGpio *gpio, JZGpio::Waveform type, int freqHz, int amplitude, int offset) override;
    void stopWaveform(JZGpio *gpio) override;

    bool setTriggerEdge(JZGpio *gpio, JZGpio::Edge edge) override;

private:
    struct PinState
    {
        JZGpio           *gpio = nullptr;
        JZGpio::Direction dir = JZGpio::Input;
        JZGpio::Pull      pull = JZGpio::PullNone;
        JZGpio::Edge      edge = JZGpio::NoEdge;
        bool              isrAdded = false;

        // PWM (LEDC)
        bool  pwmRunning = false;
        int   ledcChannel = -1;
        int   pwmFreq = 1000;
        float pwmDuty = 0.5f;

        // 波形 (独立 FreeRTOS 任务)
        bool             waveRunning = false;
        JZGpio::Waveform waveform = JZGpio::WaveNone;
        int              waveFreq = 1000;
        int              waveAmp = 4095;
        int              waveOffset = 0;
        double           wavePhase = 0.0;
        bool             useDac = false;
#if SOC_DAC_SUPPORTED
        void            *dacHandle = nullptr;  // dac_oneshot_handle_t
#endif
        int              lastLevel = -1;       // 扫描用: 上一次读取到的电平(-1 表示尚未采样)
    };

    static void isrHandler(void *arg);   // GPIO ISR (IRAM)
    static void dispatchTask(void *arg); // 队列消费任务, 运行在普通上下文
    static void waveTask(void *arg);     // 波形生成任务, 周期采样输出

    int  allocLedcChannel();
    void freeLedcChannel(int ch);
    void applyPwm(PinState &st);
    void ensureWaveTask();               // 懒启动波形任务(仅当有待生成的波形时)

    std::map<int, PinState> m_pins;   // pin -> state
    void *m_isrQueue = nullptr;       // QueueHandle_t
    void *m_dispatchTask = nullptr;   // TaskHandle_t
    void *m_waveTask = nullptr;       // TaskHandle_t
    void *m_waveMutex = nullptr;      // SemaphoreHandle_t (保护 m_pins 并发访问)
    void *m_exitSem = nullptr;        // SemaphoreHandle_t (计数信号量, 析构时等待任务退出)
    bool  m_isrServiceInstalled = false;
    bool  m_running = false;        // 整体运行标志(析构时置 false, 停止所有任务)
    bool  m_scanRunning = false;    // 扫描(dispatch)任务标志, 与波形任务相互独立
    unsigned m_ledcChannelMask = 0;   // LEDC 通道占用位图
};

#endif // !JZ_GPIO_DRIVER_ESP32_H_
