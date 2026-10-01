#include "JZFixedTimer.h"
#include "JZDebug.h"

#if defined(_WIN32)
#  include "JZThread.h"
#  include "JZMutex.h"
#  include "JZPlatform.h"
#  include <vector>
#  include <mutex>
#  include <thread>
#  include <atomic>
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#    define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002   // 老 SDK 头里没有(Win10 1803+ 才有)
#  endif

// ============================================================================
// JZFixedTimerThread —— JZFixedTimer 在 Windows 上的"闹钟线程"(全进程共享一条)
//
// 形态对标 JZNetThread(JZNetObject.cpp): 单例、继承 JZThread、自己写 run(), 跨线程一律
// 用投递而不是直接调。
//
// 职责只有一件: **到点**。每个 JZFixedTimer 只在这里登记一个"下次到期时刻", 线程睡到
// 最近的那个点到点后, 把对应的 JZFixedTimer::tick() 投回它自己的线程执行:
//   · 为什么不在这里直接 emit sigTimeout: JZObject 的线程归宿定义了它的信号从哪个线程
//     发出, 使用者(JZEmulatorView / GameWidget)的槽里直接操作窗口和模拟器, 必须留在
//     原线程; 框架其它地方也是这样钉着的(见 JZThread.cpp 的 processTimers:
//     JZ_ASSERT(t.object->thread() == this))。
//   · 于是本线程**不碰**任何定时器的状态, 补帧计算全在各自对象线程里跑 ⇒ 定时器这边无锁。
//   · 投递用 runOnObject(): 内部走 guardPointer(弱引用), 对象已析构时这次投递自动作废,
//     不会打到悬空指针上; 表里的登记项同样用弱引用, 线程顺手把失效的清掉。
//
// 唤醒用高精度可等待定时器: SetTimer/WM_TIMER 的精度取决于**系统时钟节拍**(全局的,
// 会被别的进程的 timeBeginPeriod 改变, 实测同一台机不同时刻能差出 10ms 网格与 1ms 网格
// 两种状态), 而 CreateWaitableTimerEx(CREATE_WAITABLE_TIMER_HIGH_RESOLUTION) 是 ~0.5ms
// 且不受节拍影响(Win10 1803+)。老系统不支持该标志时退回普通精度定时器(仍不比 SetTimer 差)。
// 定时器只用一个句柄、以 one-shot 方式按"距最近到期还有多久"反复重设 —— 这样登记项数量
// 不受 WaitForMultipleObjects 的 64 句柄上限约束。
//
// ⚠ 刻意**不继承 JZThread、也不注册进 jzApp 的线程表**: 它是进程级单例、活到进程结束, 而
//   JZApplication 析构时会断言"线程表必须已空"(JZApplication.cpp:
//   JZ_ASSERT(m_threads.empty())) —— 一个永不退出的 JZThread 会让那个断言失败(实测就是
//   跑完所有基准后退出时 0x80000003)。这里直接用 std::thread + detach: 不归框架管,
//   也不需要它的优先级/栈配置。
// ============================================================================
class JZFixedTimerThread
{
public:
    static JZFixedTimerThread *instance()
    {
        // 进程级单例: 不挂在 jzApp 下(免得 app 先析构, 而定时器随后还要来注销)。
        static std::mutex s_lock;
        std::lock_guard<std::mutex> lk(s_lock);
        if (!s_inst)
            s_inst = new JZFixedTimerThread();
        return s_inst;
    }

    // 登记(首次 start)或更新周期(已在表里)。可从任意对象线程调用。
    void subscribe(JZFixedTimer *timer, int intervalMs)
    {
        JZMutexLocker lock(&m_mutex);
        int64_t intervalNs = (int64_t)intervalMs * 1000000LL;
        int64_t now = JZPlatform::getTickCountNs();

        for (size_t i = 0; i < m_entries.size(); i++)
        {
            if (m_entries[i].timer.object() == timer)     // 已在表里: 只更新周期与到期点
            {
                m_entries[i].intervalNs = intervalNs;
                m_entries[i].nextDueNs = now + intervalNs;
                wakeUpThread();
                return;
            }
        }

        Entry e;
        e.timer = timer->guardPointer();
        e.intervalNs = intervalNs;
        e.nextDueNs = now + intervalNs;
        m_entries.push_back(e);
        wakeUpThread();
    }

    void unsubscribe(JZFixedTimer *timer)
    {
        JZMutexLocker lock(&m_mutex);
        for (size_t i = 0; i < m_entries.size(); i++)
        {
            if (m_entries[i].timer.object() == timer)
            {
                m_entries.erase(m_entries.begin() + i);
                break;
            }
        }
        wakeUpThread();
    }

private:
    void run()
    {
        HANDLE waits[2] = { m_hTimer, m_hWake };
        std::vector<JZFixedTimer *> due;      // 复用以少几次分配

        while (!m_stop.load())
        {
            due.clear();
            int64_t waitNs = 0;
            bool hasWait = false;

            {
                JZMutexLocker lock(&m_mutex);
                int64_t now = JZPlatform::getTickCountNs();

                for (size_t i = 0; i < m_entries.size(); )
                {
                    JZFixedTimer *t = (JZFixedTimer *)m_entries[i].timer.object();
                    if (!t)                                   // 对象已析构: 顺手清掉
                    {
                        m_entries.erase(m_entries.begin() + i);
                        continue;
                    }
                    if (m_entries[i].nextDueNs <= now)
                    {
                        due.push_back(t);
                        // ⚠ 按**绝对时间轴**推进(+= interval), 不是 now + interval:
                        //   后者会把"唤醒 -> 处理 -> 投递"的延迟每次累进下一个周期, 闹钟相位
                        //   于是持续后漂; 而 tick() 那侧的墙钟网格(m_nextEmitNs)是绝对对齐的,
                        //   ⇒ 两者相对相位在 [0, 一个步长) 里游走 —— 实测就表现为"平均偏移
                        //   恰好半个步长、极差一个步长"。
                        //   落后的那些拍不在这里数, 交给 tick() 用 behind 自己补。
                        m_entries[i].nextDueNs += m_entries[i].intervalNs;
                        if (m_entries[i].nextDueNs <= now)   // 真落后一整拍以上: 从此刻重新起算
                            m_entries[i].nextDueNs = now + m_entries[i].intervalNs;
                    }
                    i++;
                }

                for (size_t i = 0; i < m_entries.size(); i++)
                {
                    int64_t d = m_entries[i].nextDueNs - now;
                    if (!hasWait || d < waitNs)
                    {
                        waitNs = d;
                        hasWait = true;
                    }
                }
            }

            for (size_t i = 0; i < due.size(); i++)
            {
                JZFixedTimer *t = due[i];
                runOnObject(t, [t]() { t->tick(); });
            }

            if (m_stop.load())
                break;
            if (!due.empty())
                continue;              // 刚投递过, 先回头看看是不是又到点了

            if (!hasWait)
            {
                // 一个登记项都没有: 只等"有人 subscribe"的那次唤醒
                WaitForSingleObject(m_hWake, INFINITE);
                continue;
            }

            if (waitNs < 1)
                waitNs = 1;
            LARGE_INTEGER d;
            d.QuadPart = -(LONGLONG)(waitNs / 100);     // ns -> 100ns 单位; 负数 = 相对时间
            if (d.QuadPart == 0)
                d.QuadPart = -1;
            SetWaitableTimer(m_hTimer, &d, 0, nullptr, nullptr, FALSE);   // one-shot: 到时再重设
            WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        }
    }

    JZFixedTimerThread()
        : m_hTimer(nullptr), m_hWake(nullptr), m_stop(false)
    {
        m_hWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);   // 自动重置
        m_hTimer = CreateWaitableTimerExW(nullptr, nullptr,
                                          CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!m_hTimer)                                            // 旧系统: 退回普通精度
            m_hTimer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);

        m_thread = std::thread([this]() { run(); });
        m_thread.detach();   // 进程级线程: 单例不析构, 不 join; 进程结束时由系统回收
    }

    ~JZFixedTimerThread()
    {
        m_stop = true;
        wakeUpThread();
        if (m_hTimer) { CloseHandle(m_hTimer); m_hTimer = nullptr; }
        if (m_hWake)  { CloseHandle(m_hWake);  m_hWake = nullptr; }
    }

    struct Entry
    {
        JZObjectPointer timer;   // 弱引用: 对象没了自然失效, 线程顺手清理
        int64_t intervalNs;
        int64_t nextDueNs;       // 用 JZPlatform::getTickCountNs 的时间轴
    };

    void wakeUpThread()
    {
        if (m_hWake)
            SetEvent(m_hWake);
    }

    static JZFixedTimerThread *s_inst;

    HANDLE m_hTimer;
    HANDLE m_hWake;
    std::atomic<bool> m_stop;
    std::thread m_thread;        // detach 过: 析构时不必 join
    JZMutex m_mutex;
    std::vector<Entry> m_entries;
};

JZFixedTimerThread *JZFixedTimerThread::s_inst = nullptr;

#endif // _WIN32

JZFixedTimer::JZFixedTimer(JZObject *parent)
    : JZObject(parent), sigTimeout(this), sigError(this)
#if !defined(_WIN32)
    , m_timeId(INVALID_TIMER_ID)
#endif
    , m_interval(0), m_maxCatchUp(5)
    , m_nextEmitNs(0), m_frameCount(0)
{
    // Windows 下计时线程是全进程共享的单例(见 JZFixedTimerThread), 首次 start() 时才创建,
    // 所以这里什么都不用建 —— 一个没启动过的 JZFixedTimer 是零成本的。
}

JZFixedTimer::~JZFixedTimer()
{
    stop();   // 从共享计时线程注销, 免得它继续往已析构的对象投递(投递本身走弱引用, 双保险)
}

void JZFixedTimer::start(int intervalMs)
{
    start(intervalMs, m_maxCatchUp);
}

void JZFixedTimer::start(int intervalMs, int maxCatchUp)
{
    if (m_interval.load() != 0)
    {
        jzWarning() << "JZFixedTimer already started";
        return;
    }
    if (intervalMs <= 0)
    {
        jzWarning() << "JZFixedTimer interval must be > 0";
        return;
    }
    if (maxCatchUp <= 0)
    {
        jzWarning() << "JZFixedTimer maxCatchUp must be > 0";
        return;
    }

    m_maxCatchUp = maxCatchUp;
    m_frameCount = 0;

    m_elapsed.start();
    m_nextEmitNs = (int64_t)intervalMs * 1000000LL;   // 第一帧应在启动后一个步长处

    m_interval = intervalMs;     // 先发布周期: 计时线程醒来读到的就是新值
#if defined(_WIN32)
    JZFixedTimerThread::instance()->subscribe(this, intervalMs);   // 登记到共享计时线程
#else
    m_timeId = startTimer(intervalMs);
#endif
}

void JZFixedTimer::stop()
{
    if (m_interval.load() == 0)
        return;

    m_interval = 0;              // 先置 0: 这期间投递过来的 tick() 会直接返回
#if defined(_WIN32)
    JZFixedTimerThread::instance()->unsubscribe(this);   // 从共享计时线程注销
#else
    if (m_timeId != INVALID_TIMER_ID)
    {
        killTimer(m_timeId);
        m_timeId = INVALID_TIMER_ID;
    }
#endif
}

void JZFixedTimer::restart()
{
    int ms = m_interval.load();
    int maxCatchUp = m_maxCatchUp;
    stop();
    if (ms > 0)
        start(ms, maxCatchUp);
}

bool JZFixedTimer::isActive() const
{
    return m_interval.load() != 0;
}

int JZFixedTimer::interval() const
{
    return m_interval.load();
}

void JZFixedTimer::setMaxCatchUp(int frames)
{
    m_maxCatchUp = frames;
}

int JZFixedTimer::maxCatchUp() const
{
    return m_maxCatchUp;
}

int64_t JZFixedTimer::frameCount() const
{
    return m_frameCount;
}

#if !defined(_WIN32)
void JZFixedTimer::timerEvent(JZTimerEvent *event)
{
    tick();
}
#endif

void JZFixedTimer::tick()
{
    const int intervalMs = m_interval.load();
    if (intervalMs <= 0)
        return;                     // 已停(启动前就投递过来的那一拍, 或槽里刚 stop)

    // 时间基准用**纳秒**而不是毫秒(见 test/core/test_benchmark_io.cpp 的唤醒精度一档):
    // 用 ms 时"实际只差几百微秒"会被整数化算成"早于边界"从而跳过这一拍, 下一拍 behind≈一个
    // 步长再补两帧, 于是偏移在 {≈0, ≈一个步长} 之间跳 —— 5 秒统计下来均值正好是半个步长。
    const int64_t intervalNs = (int64_t)intervalMs * 1000000LL;

    int64_t now = m_elapsed.nsecsElapsed();
    if (now < m_nextEmitNs)
        return;                     // 唤醒早于下一帧边界(底层定时器略早触发), 跳过, 等下一次

    // 根据真实流逝时间计算应补发的帧数
    int64_t behind = now - m_nextEmitNs;
    int frames = (int)(behind / intervalNs) + 1;

    if (frames > m_maxCatchUp)
    {
        // 落后太多, 补不过来 -> 停止并上报
        jzWarning() << "JZFixedTimer: too many frames to catch up (" << frames
                    << " > " << m_maxCatchUp << "), stopping";
        stop();
        sigError.emit(frames);
        return;
    }

    for (int i = 0; i < frames; i++)
    {
        sigTimeout.emit();
        m_frameCount++;
        if (!isActive())   // 槽内调用了 stop()
            return;
    }

    // 对齐到墙钟而非简单累加, 避免补帧造成的漂移累积
    m_nextEmitNs += (int64_t)frames * intervalNs;
}
