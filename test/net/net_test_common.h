#pragma once
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>

#include "JZDebug.h"
#include "JZTimer.h"
#include "JZEventLoop.h"

// 各 net 模块测试共用的工具函数

inline std::string netMakeMsg(const std::string &prefix, uint64_t seq)
{
    return prefix + " " + std::to_string(seq) + "\n";
}

inline bool netParseMsg(const std::string &line, const std::string &prefix, uint64_t &seq)
{
    std::string p = prefix + " ";
    if (line.size() < p.size() || line.compare(0, p.size(), p) != 0)
        return false;
    try
    {
        seq = std::stoull(line.substr(p.size()));
    }
    catch (...)
    {
        return false;
    }
    return true;
}

// 测试是否处于活跃运行状态: 正常运行时为 true;
// 进入析构/退出阶段(事件循环已返回, 正在关闭 socket/线程)置 false,
// 避免关闭 socket 触发的 sigError/sigDisconnect 等正常事件被误判为错误而 exit(1)
inline std::atomic<bool> g_netTestActive{true};

// 任一模块发现错误立即 exit(1), 终止整个测试进程
inline void netReportError(const std::string &where, const std::string &detail)
{
    if (!g_netTestActive.load())
        return;   // 测试已结束, 关闭 socket 等正常析构事件忽略
    jzDebug() << "[FATAL][" << where << "] " << detail;
    std::exit(1);
}

// 安装一个定时器, ms 毫秒后退出事件循环
inline void netInstallExit(JZObject *owner, JZEventLoop *loop, int ms)
{
    JZTimer *t = new JZTimer(owner);
    connect(&t->sigTimeout, owner, [loop]() { loop->quit(); });
    t->start(ms);
}

// TPS 计数器: 每 1s 打印一次吞吐率并清零, 各模块共用避免重复实现
// 同时累计总消息数, 供结束时计算整体平均 TPS 用于 benchmark 判断
class NetTpsCounter : public JZObject
{
public:
    NetTpsCounter(const std::string &name, JZObject *parent = nullptr) : JZObject(parent), m_name(name) {}

    void begin()
    {
        m_start = std::chrono::steady_clock::now();
        m_totalStart = m_start;
        if (!m_timer)
            m_timer = new JZTimer(this);
        connect(&m_timer->sigTimeout, this, &NetTpsCounter::onTick);
        m_timer->start(1000);
    }

    // 停止每秒打印(测试结束时调用, 防止定时器泄漏到后续测试持续 tick)
    void stop()
    {
        if (m_timer)
            m_timer->stop();
    }

    // 累计消息数(默认 +1), 线程安全(原子)
    void hit(int n = 1) { m_count += n; m_total += n; }

    void onTick()
    {
        auto now = std::chrono::steady_clock::now();
        double el = std::chrono::duration<double>(now - m_start).count();
        jzDebug() << "[" << m_name << " TPS] " << (m_count.exchange(0) / el);
        m_start = now;
    }

    // 整体平均 TPS(从 begin 到调用时刻), 用于结束后的 benchmark 判断
    double average() const
    {
        auto now = std::chrono::steady_clock::now();
        double el = std::chrono::duration<double>(now - m_totalStart).count();
        return el > 0 ? (double)m_total / el : 0.0;
    }

    std::string m_name;
    JZTimer *m_timer = nullptr;
    std::atomic<int> m_count{0};
    std::atomic<int64_t> m_total{0};
    std::chrono::steady_clock::time_point m_start;
    std::chrono::steady_clock::time_point m_totalStart;
};

// 结束判断: 平均 TPS 是否达到阈值, 否则视为 benchmark 不达标直接 exit(1)
inline void netCheckTps(const std::string &name, double tps, double threshold = 200.0)
{
    if (tps < threshold)
        netReportError(name, "TPS 过低 " + std::to_string((int)tps) + " < " + std::to_string((int)threshold));
    jzDebug() << "[Benchmark][" << name << "] TPS = " << (int)tps << " (>= " << (int)threshold << " OK)";
}

// 默认单/多线程客户端数
#ifndef NET_TEST_THREAD_COUNT
#define NET_TEST_THREAD_COUNT 8
#endif
