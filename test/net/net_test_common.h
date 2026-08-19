#pragma once
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>

#include "JZDebug.h"
#include "JZTimer.h"
#include "JZEventLoop.h"

#define NET_TEST_THREAD_COUNT 8

struct TestOption
{
    bool enableServer = true;
    bool enableClient = true;
    int loopTime = 15000;
    int delayTime = 0;
    std::string host = "127.0.0.1";   // 客户端连接的目标服务器地址(由 --server 指定)
};

// 各 net 模块测试共用的工具函数
std::string netMakeMsg(const std::string &prefix, uint64_t seq);

bool netParseMsg(const std::string &line, const std::string &prefix, uint64_t &seq);

// 测试是否处于活跃运行状态: 正常运行时为 true;
// 进入析构/退出阶段(事件循环已返回, 正在关闭 socket/线程)置 false,
// 避免关闭 socket 触发的 sigError/sigDisconnect 等正常事件被误判为错误而 exit(1)
extern std::atomic<bool> g_netTestActive;

// 任一模块发现错误立即 exit(1), 终止整个测试进程
void netReportError(const std::string &where, const std::string &detail);

// 安装一个定时器, ms 毫秒后退出事件循环
void netInstallExit(JZObject *owner, JZEventLoop *loop, int ms);

// 纯服务器模式生命周期管理(纯服务器时使用):
// - 启动后等待首个客户端连接, 超过 waitMs 仍未连接则退出(watchdog)
// - 首个连接到达后(需调用方在 onAccept 中调用返回的 bench->onConnected()),
//   再运行 runMs 然后退出事件循环
class NetServerBench;
NetServerBench *serverInstallExit(JZObject *owner, JZEventLoop *loop, int waitMs, int runMs);

// TPS 计数器: 每 1s 打印一次吞吐率并清零, 各模块共用避免重复实现
// 同时累计总消息数, 供结束时计算整体平均 TPS 用于 benchmark 判断
class NetTpsCounter : public JZObject
{
public:
    NetTpsCounter(const std::string &name, JZObject *parent = nullptr);

    void begin();

    // 停止每秒打印(测试结束时调用, 防止定时器泄漏到后续测试持续 tick)
    void stop();

    // 累计消息数(默认 +1), 线程安全(原子)
    void hit(int n = 1);

    void onTick();

    // 整体平均 TPS = 总消息数 / g_option.loopTime(秒), 用于结束后的 benchmark 判断
    double average() const;

    // 累计总消息数
    int64_t total() const { return m_total.load(); }

    std::string m_name;
    JZTimer *m_timer = nullptr;
    std::atomic<int> m_count{0};
    std::atomic<int64_t> m_total{0};
    std::chrono::steady_clock::time_point m_start;
};

// 服务器模式生命周期管理(纯服务器时使用):
// - 启动后等待首个客户端连接, 超过 waitMs 仍未连接则退出(watchdog)
// - 首个连接到达后, 再运行 runMs 然后退出
class NetServerBench : public JZObject
{
public:
    NetServerBench(JZObject *owner, JZEventLoop *loop, int waitMs, int runMs)
        : m_loop(loop)
    {
        (void)owner;
        m_timer = new JZTimer(this);
        m_timer->setSingleShot(true);
        connect(&m_timer->sigTimeout, this, &NetServerBench::onTimeout);
        m_timer->start(waitMs);
    }

    // 首个客户端连接时调用, 切换到 runMs 退出计时
    void onConnected()
    {
        if (m_hasConnected.exchange(true))
            return;   // 仅首次
        m_timer->stop();
        m_timer->start(m_runMs);
    }

    bool hasConnected() const { return m_hasConnected.load(); }

    // 测试结束(loop.exec 返回)后必须调用, 停止定时器并释放自身,
    // 否则 m_timer 仍挂在主线程 timerList, 主线程析构后可能再次触发 onTimeout
    // -> m_loop(栈上 JZEventLoop)已析构 -> JZThread::wakeUp 访问已释放的 m_cv 段错误
    void stop()
    {
        if (m_timer)
        {
            m_timer->stop();
            delete m_timer;
            m_timer = nullptr;
        }
        delete this;
    }

private:
    ~NetServerBench()
    {
        if (m_timer)
        {
            m_timer->stop();
            delete m_timer;
            m_timer = nullptr;
        }
    }

    void onTimeout()
    {
        if (m_loop)
            m_loop->quit();
    }

    JZEventLoop *m_loop;
    int m_runMs = 20000;
    JZTimer *m_timer = nullptr;
    std::atomic<bool> m_hasConnected{false};
};

// 结束判断: 平均 TPS 是否达到阈值, 否则视为 benchmark 不达标直接 exit(1)
void netCheckTps(const std::string &name, double tps, double threshold = 200.0);
extern TestOption g_option;