#ifndef JZ_GPIO_DRIVER_H_
#define JZ_GPIO_DRIVER_H_

#include <vector>
#include "JZGpio.h"
#include "JZGpioInfo.h"

// GPIO 驱动抽象基类(单例)。具体平台(ESP32 / 模拟器 / 主机)实现自己的子类,
// 并在程序初始化时通过 setInstance() 安装。JZGpio 的所有底层操作都委派给它。
class JZGpioDriver
{
public:
    JZGpioDriver() = default;
    virtual ~JZGpioDriver() = default;

    static JZGpioDriver *instance();
    static void setInstance(JZGpioDriver *inst);
    static void clearInstance();

    // 枚举当前平台可用 GPIO 引脚。各平台驱动自行实现。
    virtual std::vector<JZGpioInfo> availableGpios() = 0;

    // 打开 / 关闭 / 查询
    virtual bool open(JZGpio *gpio) = 0;
    virtual void close(JZGpio *gpio) = 0;
    virtual bool isOpen(JZGpio *gpio) = 0;

    // 配置
    virtual bool setDirection(JZGpio *gpio, JZGpio::Direction dir) = 0;
    virtual bool setPull(JZGpio *gpio, JZGpio::Pull pull) = 0;

    // 数字 IO
    virtual bool write(JZGpio *gpio, int level) = 0;
    virtual int  read(JZGpio *gpio) = 0;

    // PWM
    virtual bool startPwm(JZGpio *gpio, int freqHz, float duty) = 0;
    virtual bool setPwmFrequency(JZGpio *gpio, int freqHz) = 0;
    virtual bool setPwmDuty(JZGpio *gpio, float duty) = 0;
    virtual void stopPwm(JZGpio *gpio) = 0;

    // 模拟波形
    virtual bool startWaveform(JZGpio *gpio, JZGpio::Waveform type, int freqHz, int amplitude, int offset) = 0;
    virtual void stopWaveform(JZGpio *gpio) = 0;

    // 触发(输入中断边沿)
    virtual bool setTriggerEdge(JZGpio *gpio, JZGpio::Edge edge) = 0;

protected:
    JZ_DISABLE_COPY(JZGpioDriver)

    static JZGpioDriver *s_instance;
};

#endif // !JZ_GPIO_DRIVER_H_
