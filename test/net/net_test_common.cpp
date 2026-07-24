#include "net_test_common.h"

std::string netMakeMsg(const std::string &prefix, uint64_t seq)
{
    return prefix + " " + std::to_string(seq) + "\n";
}

bool netParseMsg(const std::string &line, const std::string &prefix, uint64_t &seq)
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

std::atomic<bool> g_netTestActive{true};

void netReportError(const std::string &where, const std::string &detail)
{
    if (!g_netTestActive.load())
        return;   // 测试已结束, 关闭 socket 等正常析构事件忽略
    jzDebug() << "[FATAL][" << where << "] " << detail;
    std::exit(1);
}

void netInstallExit(JZObject *owner, JZEventLoop *loop, int ms)
{
    JZTimer *t = new JZTimer(owner);
    connect(&t->sigTimeout, owner, [loop]() { loop->quit(); });
    t->start(ms);
}

NetTpsCounter::NetTpsCounter(const std::string &name, JZObject *parent)
    : JZObject(parent), m_name(name)
{
}

void NetTpsCounter::begin()
{
    m_start = std::chrono::steady_clock::now();
    m_totalStart = m_start;
    if (!m_timer)
        m_timer = new JZTimer(this);
    connect(&m_timer->sigTimeout, this, &NetTpsCounter::onTick);
    m_timer->start(1000);
}

void NetTpsCounter::stop()
{
    if (m_timer)
        m_timer->stop();
}

void NetTpsCounter::hit(int n)
{
    m_count += n;
    m_total += n;
}

void NetTpsCounter::onTick()
{
    auto now = std::chrono::steady_clock::now();
    double el = std::chrono::duration<double>(now - m_start).count();
    jzDebug() << "[" << m_name << " TPS] " << (m_count.exchange(0) / el);
    m_start = now;
}

double NetTpsCounter::average() const
{
    auto now = std::chrono::steady_clock::now();
    double el = std::chrono::duration<double>(now - m_totalStart).count();
    return el > 0 ? (double)m_total / el : 0.0;
}

void netCheckTps(const std::string &name, double tps, double threshold)
{
    if (tps < threshold)
        netReportError(name, "TPS 过低 " + std::to_string((int)tps) + " < " + std::to_string((int)threshold));
    jzDebug() << "[Benchmark][" << name << "] TPS = " << (int)tps << " (>= " << (int)threshold << " OK)";
}
