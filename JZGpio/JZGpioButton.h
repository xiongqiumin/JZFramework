#ifndef JZ_GPIO_BUTTON_H_
#define JZ_GPIO_BUTTON_H_
#include "JZObject.h"
#include "JZGpio.h"
#include "JZGpioInfo.h"
#include "JZTimer.h"

// JZGpioButton 是基于 JZGpio 的按键抽象。
// 它把一个输入引脚封装成"按键", 负责消抖并识别:
//   按下(pressed) / 释放(released) / 单击(clicked) /
//   双击(doubleClicked) / 长按(longPressed)
//
// 用法:
//   // 方式一: 传入引脚号, 内部自动创建并管理 JZGpio
//   auto *btn = new JZGpioButton(0);
//   btn->setPull(JZGpio::PullUp);       // 通常按键用上拉, 按下为 Low
//   connect(&btn->sigClicked, []{ ... });
//
//   // 方式二: 传入 JZGpioInfo(来自 availableGpios())
//   auto infos = JZGpio::availableGpios();
//   auto *btn2 = new JZGpioButton(infos[0]);
//
// 说明:
//   - 传入 pin / JZGpioInfo 时, 按钮内部创建 JZGpio 并以自身为父对象托管。
//   - 构造时自动配置为输入、开启监控(setMonitorEnabled(true))并连接
//     sigLevelChanged, 随后打开引脚(若尚未打开)。
//   - 时间基准使用 JZPlatform::getTickCountMs()。
class JZFRAMEWORK_API JZGpioButton : public JZObject
{
public:
    // 传入引脚号, 内部创建并管理 JZGpio
    explicit JZGpioButton(int pin, JZObject *parent = nullptr);
    // 传入引脚信息, 内部创建并管理 JZGpio
    explicit JZGpioButton(const JZGpioInfo &info, JZObject *parent = nullptr);
    virtual ~JZGpioButton();

    // 有效电平: 按键被按下时引脚应处的电平(默认 Low, 即上拉按键)。
    void setActiveLevel(int level);
    int  activeLevel() const;

    // 上下拉配置(透传给内部 JZGpio, 仅在传入 pin/info 时有效)。
    void setPull(JZGpio::Pull pull);
    JZGpio::Pull pull() const;

    // 消抖窗口(ms), 默认 30
    void setDebounceMs(int ms);
    int  debounceMs() const;

    // 长按阈值(ms), 默认 500
    void setLongPressMs(int ms);
    int  longPressMs() const;

    // 双击判定窗口(ms), 默认 300
    void setDoubleClickMs(int ms);
    int  doubleClickMs() const;

    // 当前是否处于按下状态
    bool isPressed() const;

jz_signals:
    JZSigVoid sigPressed;        // 按下(消抖后)
    JZSigVoid sigReleased;       // 释放(消抖后)
    JZSigVoid sigClicked;        // 单击(短按后释放, 且无第二击)
    JZSigVoid sigDoubleClicked;  // 双击
    JZSigVoid sigLongPressed;    // 长按到达阈值

protected:
    void setupGpio(JZGpio *gpio);
    void onLevelChanged(int level);
    void onTimer();

    enum TimerMode {
        TimerNone = 0,
        TimerLongPress,   // 长按计时
        TimerClickWait,   // 等待可能的第二击
    };

    JZGpio   *m_gpio = nullptr;
    int       m_activeLevel = JZGpio::Low;
    int       m_debounceMs = 30;
    int       m_longPressMs = 500;
    int       m_doubleClickMs = 300;

    bool      m_pressed = false;
    int64_t   m_pressStartMs = 0;
    int64_t   m_lastStableMs = 0;   // 上次有效(消抖后)翻转时刻
    int64_t   m_lastReleaseMs = 0;  // 上次释放时刻, 用于双击判定
    bool      m_longFired = false;
    bool      m_waitDouble = false;

    TimerMode m_timerMode = TimerNone;
    JZTimer   *m_timer = nullptr;
};

#endif // !JZ_GPIO_BUTTON_H_
