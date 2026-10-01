#ifndef JZ_FIXED_TIMER_H_
#define JZ_FIXED_TIMER_H_
#include "JZObject.h"
#include "JZElapsedTimer.h"
#include <atomic>

#if defined(_WIN32)
class JZFixedTimerThread;
#endif

// JZFixedTimer: 以固定步长(墙钟间隔)触发 sigTimeout 的定时器。
//
// 与 JZTimer 的区别: JZTimer 每次定时器事件只触发一次, 若事件回路被阻塞
// 或回调耗时超过间隔, 触发频率会自然下降(慢了就慢了)。JZFixedTimer 以"墙钟"
// 为基准, 每当底层定时器唤醒时, 根据真实流逝时间计算应当补发的帧数并连续补帧,
// 从而尽可能维持固定的总触发频率(类似游戏主循环的固定步长)。两者配合: 唤醒偏晚
// 由补帧找回来, 时间不漂移。
//
// 唤醒(仅 Windows 与其它平台不同):
//   · Windows: 全进程**共享一条计时线程**(单例, 见 .cpp 的 JZFixedTimerThread —— 形态对标
//     JZNetThread): 每个 JZFixedTimer 只往它那里登记一个"下次到期时刻", 它在本线程里用
//     高精度可等待定时器 one-shot 睡到最近的那个点到点(CreateWaitableTimerEx +
//     CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, ~0.5ms, 不受系统时钟节拍影响), 到点后把补帧
//     动作**投回各自对象所属线程**执行 ⇒ 线程归宿不变(sigTimeout 仍从你创建它的那个线程
//     发出), 计时线程不碰任何定时器的状态 ⇒ 定时器这边不需要锁。
//     所以 N 个 JZFixedTimer 也只有一条线程、一个定时器句柄, 不是每个实例一条。
//     ⚠ 之所以在 Windows 上单开一条路: SetTimer/WM_TIMER 的唤醒精度依赖**系统时钟
//     节拍**(该节拍是全局的, 会被别的进程的 timeBeginPeriod 改变), 实测同一台机不同
//     时刻能差出 10ms 网格与 1ms 网格两种状态; 高精度定时器不受这个影响。
//   · 其它平台(嵌入式/Linux): 仍由框架定时器驱动(与改造前完全一致, 不引入线程),
//     唤醒精度就是平台时钟粒度 —— 这些平台上 tick 本来就是天花板, 换机制不会更准。
//
// 防死亡螺旋: 若单次落后帧数超过 maxCatchUp, 认为系统已无法跟上, 立即 stop()
// 并通过 sigError(落后帧数) 上报, 避免无限补帧拖垮系统。
class JZFRAMEWORK_API JZFixedTimer : public JZObject
{
public:
    JZFixedTimer(JZObject *parent = nullptr);
    virtual ~JZFixedTimer();

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
#if !defined(_WIN32)
    // 非 Windows: 由框架定时器按步长唤醒(与改造前一致), 醒来后补帧。
    virtual void timerEvent(JZTimerEvent *event) override;
#endif

private:
#if defined(_WIN32)
    friend class JZFixedTimerThread;
#endif

    // 补帧。Windows 上由计时线程投回本对象所属线程后执行; 其它平台由 timerEvent 调。
    // 只读原子的 m_interval, 其余成员(m_elapsed/m_nextEmitNs/m_frameCount)只在本线程读写。
    void tick();

#if !defined(_WIN32)
    int m_timeId;                        // 框架定时器句柄(Windows 走共享计时线程, 无实例成员)
#endif
    std::atomic<int> m_interval;         // 当前步长(ms); 0 = 未启动。计时线程也要读, 故用原子
    int m_maxCatchUp;                    // 单次最多补帧数
    int64_t m_nextEmitNs;                // 下一次应发射的墙钟偏移(纳秒, 相对启动时刻), 不因补帧漂移
    int64_t m_frameCount;                // 已发射帧数
    JZElapsedTimer m_elapsed;
};

#endif // !JZ_FIXED_TIMER_H_
