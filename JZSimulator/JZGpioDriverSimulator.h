#ifndef JZ_GPIO_DRIVER_SIMULATOR_H_
#define JZ_GPIO_DRIVER_SIMULATOR_H_

#include "JZGpioDriver.h"
#include "JZSimServer.h"
#include "JZObject.h"
#include "JZSimPack.h"
#include "JZSignal.h"
#include "JZTimer.h"
#include <map>
#include <string>

class JZNetPack;

// 基于 JZNetClient 的 GPIO 驱动模拟实现。
// 数字 IO / 触发 通过 JZNetClient 发往 JZSimulatorDevice (运行 JZNetServer + LVGL 界面),
// 由后者模拟真实引脚行为并把电平事件回传; 波形则由本驱动借助 JZTimer 在本地生成
// (周期采样 -> 计算电平 -> onLevelChanged), 无需依赖远端设备。
// 应用侧可像使用 Esp32 驱动一样使用它。
class JZGpioDriverSimulator : public JZGpioDriver, public JZSimulator, public JZObject
{
    JZ_OBJECT_DECLARE(JZGpioDriverSimulator)
public:
    JZGpioDriverSimulator(const std::string &ip = "127.0.0.1", int port = 19123);
    ~JZGpioDriverSimulator() override;

    // 取当前 GPIO 驱动单例(仅当其为模拟驱动时返回, 否则 nullptr)
    static JZGpioDriverSimulator *simInstance();

    // JZSimulator 接口: 下行路由判断 / 下行处理。
    bool support(JZSimCmd cmd) const override;
    void onRecv(const JZSimPack &pack) override;

    // 每次本驱动与模拟器交互(sendSim 发出指令 / onRecv 收到回发)都会通过此信号广播,
    // 上层或测试代码可 connect 订阅以观测交互。
    JZSignal<JZSimCmd, const JZVariantMap&> sigSim{this};

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
    void sendSim(JZSimCmd cmd, const JZVariantMap &m);

    // 波形本地生成
    struct WaveState
    {
        JZGpio::Waveform type = JZGpio::WaveNone;
        int  freq = 1000;
        int  amp = 1;
        int  offset = 0;
        double phase = 0.0;
    };
    void onWaveTimeout();          // JZTimer 超时: 推进所有活动波形并推送给用户
    void updateWaveTimer();        // 依据活动波形重算定时器周期(每个周期采样 ~32 点)

    JZSimServer *m_sim = nullptr;
    JZTimer      *m_waveTimer = nullptr;  // 波形生成定时器
    int          m_waveTimerMs = 5;      // 当前定时器周期(ms)

    std::map<int, JZGpio *> m_pins;       // pin -> gpio
    std::map<int, int>      m_levelCache; // pin -> 最近电平(供同步 read 返回)
    std::map<int, WaveState> m_wavePins;  // pin -> 正在生成的波形
};

#endif // !JZ_GPIO_DRIVER_SIMULATOR_H_
