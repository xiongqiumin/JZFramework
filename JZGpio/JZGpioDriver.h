#ifndef JZ_GPIO_DRIVER_H_
#define JZ_GPIO_DRIVER_H_
#include <vector>
#include "JZGpio.h"
#include "JZGpioInfo.h"

// GPIO 驱动抽象基类。支持注册多个驱动实例(如 MCU 原生 GPIO + I2C 扩展 GPIO),
// 通过 addInstance() 注册, JZGpio 按引脚号查找对应驱动。
class JZFRAMEWORK_API JZGpioDriver
{
public:
    JZGpioDriver() = default;
    virtual ~JZGpioDriver() = default;

    static void addInstance(JZGpioDriver *inst);
    static void clearInstances();
    static std::vector<JZGpioDriver*> instances();

    // 根据引脚号查找驱动, 找不到返回 nullptr
    static JZGpioDriver *findDriver(int pin);

    // 汇总所有驱动的可用引脚列表
    static std::vector<JZGpioInfo> allAvailableGpios();

    // 查询本驱动是否支持指定引脚
    virtual bool supportsPin(int pin) = 0;

    // 枚举本驱动可用 GPIO 引脚
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

    static std::vector<JZGpioDriver*> s_instances;
};

#endif // !JZ_GPIO_DRIVER_H_
