#ifndef JZ_ABSTRACT_ANIMATION_H_
#define JZ_ABSTRACT_ANIMATION_H_

#include "JZSignal.h"
#include "JZObject.h"

// 动画插值回调 (LVGL lv_anim_exec_xcb 风格): 每帧收到当前整数值
typedef void (*JZAnimExecCb)(void* target, int32_t value);

// 缓动曲线
enum class JZEasing
{
    Linear,     // 匀速
    EaseIn,     // 加速
    EaseOut,    // 减速
    EaseInOut,  // 先加速后减速 (默认)
    Overshoot,  // 过头一点再回弹 (easeOutBack)
    Bounce,     // 落地弹跳 (easeOutBounce)
};

class JZWidget;

// 帧驱动的基础补间动画 (Qt QAbstractAnimation 的 int32 简化版)。
// 计时起点 = start() 时刻 + delay; 期间按缓动曲线在 [startValue, endValue]
// 上插值, 每帧经 onValueChanged() 输出 —— 默认派发给 setExecCb 回调,
// 派生类 (如 JZPropertyAnimation) 覆盖 onValueChanged 驱动具体属性。
// 驱动方式: 内部 startTimer(16ms), 由线程定时器逐帧推进。
class JZAbstractAnimation : public JZObject
{
    JZ_OBJECT_DECLARE(JZAbstractAnimation)

public:
    JZAbstractAnimation(JZObject* parent = nullptr);
    ~JZAbstractAnimation();

    // 目标: 可以是任意对象 (void*) 或控件; setExecCb 回调首参即此目标
    void setTarget(void* obj);
    void setTarget(JZWidget* widget);
    void* target() const { return m_target; }

    bool targetIsWidget() const { return m_targetIsWidget; }
    JZWidget* targetWidget() const
    {
        return m_targetIsWidget ? static_cast<JZWidget*>(m_target) : nullptr;
    }

    void setDuration(uint32_t ms);
    void setDelay(uint32_t ms);

    void setEasingLinear();
    void setEasingEaseIn();
    void setEasingEaseOut();
    void setEasingEaseInOut();
    void setEasingOvershoot();
    void setEasingBounce();
    void setEasing(JZEasing e) { m_easing = e; }

    // 逐帧数值输出回调 (值与动画插值一致); 与派生 onValueChanged 二选一
    void setExecCb(JZAnimExecCb cb);

    // 插值范围
    void setStartValue(int32_t v);
    void setEndValue(int32_t v);
    void setRange(int32_t from, int32_t to);
    int32_t startValue() const { return m_from; }
    int32_t endValue() const { return m_to; }

    int32_t value() const { return m_curValue; }
    bool isRunning() const { return m_running; }

    void start();
    void stop();

jz_signals:
    JZSigVoid sigFinished;

protected:
    // 每帧回调: 默认交给 execCb; 派生类覆盖以驱动具体属性
    virtual void onValueChanged(int32_t value);

    void timerEvent(JZTimerEvent* event) override;

private:
    void tick(int64_t nowMs);   // 单帧推进
    void finish();              // 收尾: 落在终点值 + sigFinished
    void applyValue(int32_t v); // 记录当前值并输出一帧

    void* m_target = nullptr;
    bool  m_targetIsWidget = false;
    uint32_t m_duration = 300;
    uint32_t m_delay = 0;
    int32_t m_from = 0;
    int32_t m_to = 0;
    int32_t m_curValue = 0;
    int m_timerId = -1;
    int64_t m_beginMs = 0;      // 计时起点 = start() 时刻 + delay
    bool m_running = false;
    JZEasing m_easing = JZEasing::EaseInOut;
    JZAnimExecCb m_execCb = nullptr;
};

#endif
