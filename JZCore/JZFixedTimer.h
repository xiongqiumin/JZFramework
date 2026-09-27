#ifndef JZ_FIXED_TIMER_H_
#define JZ_FIXED_TIMER_H_
#include "JZObject.h"
#include "JZElapsedTimer.h"

// JZFixedTimer: 以固定步长(墙钟间隔)触发 sigTimeout 的定时器。
//
// 与 JZTimer 的区别: JZTimer 每次定时器事件只触发一次, 若事件回路被阻塞
// 或回调耗时超过间隔, 触发频率会自然下降(慢了就慢了)。JZFixedTimer 以"墙钟"
// 为基准, 每当底层定时器唤醒时, 根据真实流逝时间计算应当补发的帧数并连续补帧,
// 从而尽可能维持固定的总触发频率(类似游戏主循环的固定步长)。
//
// 防死亡螺旋: 若单次落后帧数超过 maxCatchUp, 认为系统已无法跟上, 立即 stop()
// 并通过 sigError(落后帧数) 上报, 避免无限补帧拖垮系统。
class JZFRAMEWORK_API JZFixedTimer : public JZObject
{
public:
    JZFixedTimer(JZObject *parent = nullptr);
    ~JZFixedTimer();

    // 以固定间隔 intervalMs(ms) 启动; maxCatchUp 为单次最多补帧数, 超过则停止并触发 sigError。
    void start(int intervalMs);
    void start(int intervalMs, int maxCatchUp);
    void stop();
    void restart();

    bool isActive() const;
    int interval() const;

    // 单次最多补帧数(默认 5)。
    void setMaxCatchUp(int frames);
    int maxCatchUp() const;

    // 累计已发射的帧数(含补帧)。
    int64_t frameCount() const;

jz_signals:
    JZSigVoid sigTimeout;   // 每个固定步长触发一次(可能有连续多次补帧)
    JZSigInt  sigError;     // 落后帧数超过阈值, 停止前触发, 参数为落后帧数

protected:
    virtual void timerEvent(JZTimerEvent *event) override;

    int m_timeId;
    int m_interval;         // 固定步长(ms)
    int m_maxCatchUp;       // 单次最多补帧数
    int64_t m_nextEmitMs;   // 下一次应发射的墙钟偏移(ms, 相对启动时刻), 不因补帧漂移
    int64_t m_frameCount;   // 已发射帧数
    JZElapsedTimer m_elapsed;
};

#endif // !JZ_FIXED_TIMER_H_
