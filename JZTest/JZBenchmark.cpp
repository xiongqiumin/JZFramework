#include "JZBenchmark.h"
#include <iostream>
#include <algorithm>
#include <climits>

using namespace std;

JZBenchmark::JZBenchmark(int targetMs, double minSec, double maxSec)
    : m_targetMs(targetMs)
    , m_minSec(minSec)
    , m_maxSec(maxSec)
{
}

void JZBenchmark::setTargetMs(int ms)
{
    m_targetMs = ms;
}

void JZBenchmark::setTimeBounds(double minSec, double maxSec)
{
    m_minSec = minSec;
    m_maxSec = maxSec;
}

std::string JZBenchmark::formatNs(int64_t ns)
{
    if (ns < 1000)
        return to_string(ns) + " ns";
    if (ns < 1000000)
        return to_string(ns / 1000.0) + " us";
    if (ns < 1000000000LL)
        return to_string(ns / 1000000.0) + " ms";
    return to_string(ns / 1000000000.0) + " s";
}

JZBenchmarkResult JZBenchmark::run(const std::string& name, const std::function<void()>& func)
{
    const int kWarmup = 5;        // 预热次数: 消除冷启动/缓存未命中
    const int kCalibration = 10;  // 校准次数: 估算单次耗时

    // 1) 预热
    for (int i = 0; i < kWarmup; ++i)
        func();

    // 2) 校准: 估算单次耗时
    JZElapsedTimer calTimer;
    calTimer.start();
    for (int i = 0; i < kCalibration; ++i)
        func();
    int64_t calNs = calTimer.nsecsElapsed();
    int64_t perIter = calNs / kCalibration;
    if (perIter < 1)
        perIter = 1; // 防除零 (极快操作)

    // 3) 自适应推迭代数: 目标总耗时 / 单次耗时, 并保证总时长落在 [minSec, maxSec]
    //    采用「整批计量」: 把 iters 分成 kRep 个批次, 每批只计时一次, 从而
    //    避免对每次迭代单独计时的开销 (快操作下计时调用本身会显著污染结果)。
    int64_t targetNs = (int64_t)m_targetMs * 1000000LL;

    // 折算时间上下限 (秒 -> 纳秒)
    int64_t minNsBound = (int64_t)(m_minSec * 1000000000.0);
    int64_t maxNsBound = (int64_t)(m_maxSec * 1000000000.0);

    // 动态批次数: 当单次耗时已接近/超过目标时, 减少批次, 避免总时长被放大数倍。
    // 例如单次约 381ms 的操作: 固定 3 批会跑 1.14s, 缩到 1 批后仅需约 381ms(单次极限)。
    const int kRepMax = 3;
    int kRep = kRepMax;
    if (perIter >= targetNs)
        kRep = 1;                                       // 单次比目标还久, 只跑 1 批(=1 次)
    else if (perIter * kRepMax > targetNs)
        kRep = max(1, (int)(targetNs / perIter));       // 批次数匹配目标, 而非固定 3

    int64_t iters = targetNs / (perIter * kRep); // 每批迭代数
    if (iters < 1) iters = 1;

    // 至少跑到 minSec: 不足则向上取整增加迭代次数
    if (iters * kRep * perIter < minNsBound)
        iters = (minNsBound + perIter * kRep - 1) / (perIter * kRep); // 向上取整
    // 至多跑到 maxSec: 超出则向下取整减少迭代次数
    if (iters * kRep * perIter > maxNsBound)
        iters = maxNsBound / (perIter * kRep);

    if (iters > INT_MAX)
        iters = INT_MAX;
    iters = max((int64_t)1, iters);

    // 4) 正式测量: 整批计时 (每批仅一次 start/stop), 统计 total 与批次 min/max
    int64_t totalNs = 0;
    int64_t minBatchNs = INT64_MAX;
    int64_t maxBatchNs = 0;
    for (int r = 0; r < kRep; ++r)
    {
        JZElapsedTimer t;
        t.start();
        for (int64_t i = 0; i < iters; ++i)
            func();
        int64_t d = t.nsecsElapsed();
        totalNs += d;
        if (d < minBatchNs) minBatchNs = d;
        if (d > maxBatchNs) maxBatchNs = d;
    }

    JZBenchmarkResult r;
    r.name = name;
    r.iters = iters * kRep;          // 总迭代次数
    r.totalNs = totalNs;             // 实际总耗时 (kRep 批之和)
    r.minNs = (iters > 0) ? minBatchNs / iters : 0; // 由最快批次估算单次最小
    r.maxNs = (iters > 0) ? maxBatchNs / iters : 0; // 由最慢批次估算单次最大
    return r;
}

void JZBenchmark::printResult(const JZBenchmarkResult& r)
{
    cout << "  [bench] " << r.name << endl
         << "    iters : " << r.iters << endl
         << "    total : " << formatNs(r.totalNs) << endl
         << "    avg   : " << formatNs((int64_t)r.avgNs()) << endl
         << "    min   : " << formatNs(r.minNs) << endl
         << "    max   : " << formatNs(r.maxNs) << endl;
}

void JZBenchmark::runAndPrint(const std::string& name, const std::function<void()>& func)
{
    printResult(run(name, func));
}
