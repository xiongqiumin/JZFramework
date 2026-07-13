#include "JZCoWatch.h"
#include "JZCo.h"
#include "JZDebug.h"

JZCoWatch::JZCoWatch(JZObject *parent)
    : JZObject(parent)
{
    // 监听全局调度器的协程结束信号(主线程直连, 不需要 JZVariant 注册参数类型)
    JZ_ASSERT(g_scheduler);
    connect(&g_scheduler->sigFinished, this, &JZCoWatch::onFinished, JZ::DirectConnection);
}

JZCoWatch::~JZCoWatch()
{
}

void JZCoWatch::blockWait()
{
    if (g_scheduler->isInCoroutine())
        m_cond.wait();    // 协程内: 让出, 由 onFinished 唤醒(调度出去, 不阻塞主线程)
    else
        m_loop.exec();    // 主线程: 事件循环驱动
}

void JZCoWatch::wakeWait()
{
    if (g_scheduler->isInCoroutine())
        m_cond.wakeOne(); // 协程内: 唤醒等待的协程
    else
        m_loop.quit();    // 主线程: 退出事件循环
}

void JZCoWatch::onFinished(JZCoCoroutine *co)
{
    // 监听整个调度器: 每收到一个结束信号就判断是否所有协程都已结束
    // (此时结束的 co 已置 m_finish=true, 但尚未从 coroutineList 移除)
    if (m_mode == Mode::Scheduler)
    {
        for (auto &p : g_scheduler->coroutineList())
        {
            if (!p->isFinish())
                return;
        }
        wakeWait();
        return;
    }

    // 只处理当前监听列表内的协程
    bool watched = false;
    for (auto &p : m_watchList)
    {
        if (p.get() == co)
        {
            watched = true;
            break;
        }
    }
    if (!watched)
        return;

    if (m_mode == Mode::Any)
    {
        // 先到先得: 记录第一个结束的协程并唤醒等待者
        if (!m_anyFinished)
            m_anyFinished = co;
        wakeWait();
    }
    else if (m_mode == Mode::All)
    {
        // 从监听列表移除已结束的协程, 全部结束则唤醒等待者
        for (int i = 0; i < (int)m_watchList.size(); i++)
        {
            if (m_watchList[i].get() == co)
            {
                m_watchList.erase(m_watchList.begin() + i);
                break;
            }
        }
        if (m_watchList.empty())
            wakeWait();
    }
}

JZCoCoroutinePtr JZCoWatch::waitAny(const std::vector<JZCoCoroutinePtr> &co_list)
{
    m_watchList = co_list;
    if (m_watchList.empty())
        return nullptr;

    // 已有结束的, 直接返回, 无需等待
    for (auto &p : m_watchList)
    {
        if (p->isFinish())
            return p;
    }

    m_mode = Mode::Any;
    m_anyFinished = nullptr;
    blockWait();
    m_mode = Mode::None;

    if (!m_anyFinished)
        return nullptr;
    for (auto &p : m_watchList)
    {
        if (p.get() == m_anyFinished)
            return p;
    }
    return nullptr;
}

void JZCoWatch::waitAll(const std::vector<JZCoCoroutinePtr> &co_list)
{
    m_watchList = co_list;
    // 先剔除已经结束的协程
    for (int i = (int)m_watchList.size() - 1; i >= 0; i--)
    {
        if (m_watchList[i]->isFinish())
            m_watchList.erase(m_watchList.begin() + i);
    }

    if (m_watchList.empty())
        return;

    m_mode = Mode::All;
    blockWait();
    m_mode = Mode::None;
}

void JZCoWatch::waitScheduler()
{
    // 调度器已无未结束协程, 直接返回
    bool all_finish = true;
    for (auto &p : g_scheduler->coroutineList())
    {
        if (!p->isFinish())
        {
            all_finish = false;
            break;
        }
    }
    if (all_finish)
        return;

    m_mode = Mode::Scheduler;
    blockWait();
    m_mode = Mode::None;
}
