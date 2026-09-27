#ifndef JZBENCHMARK_H
#define JZBENCHMARK_H
#include <string>
#include <functional>
#include <cstdint>
#include "JZElapsedTimer.h"

// 单次 benchmark 的结果
struct JZFRAMEWORK_API JZBenchmarkResult
{
    std::string name;     // 名称
    int64_t iters = 0;    // 实际运行次数 (自适应)
    int64_t totalNs = 0;  // 总耗时 (ns)
    int64_t minNs = 0;    // 单次最小耗时 (ns)
    int64_t maxNs = 0;    // 单次最大耗时 (ns)

    double avgNs() const { return iters > 0 ? double(totalNs) / double(iters) : 0.0; }
};

// 自适应 benchmark: 先预热 + 校准估算单次耗时, 再据目标耗时反推迭代数,
// 使总测试时间稳定在 targetMs 附近, 同时总时长被限制在 [minSec, maxSec] 内。
// 测量采用「整批计时」: 把迭代分批, 每批只计时一次, 避免对快操作逐次计时的开销。
class JZFRAMEWORK_API JZBenchmark
{
public:
    // targetMs : 期望的总测试时间 (毫秒), 用于推算迭代数
    // minSec   : 最少测试时长 (秒), 即使单次极快也至少跑这么久
    // maxSec   : 最多测试时长 (秒), 即使单次极慢也不会超过此值, 防止卡死
    explicit JZBenchmark(int targetMs = 100, double minSec = 0.01, double maxSec = 2.0);

    // 运行一次自适应 benchmark, 返回统计结果
    JZBenchmarkResult run(const std::string& name, const std::function<void()>& func);

    // 运行并打印结果
    void runAndPrint(const std::string& name, const std::function<void()>& func);

    // 设置目标耗时(毫秒)与测试时长上下限(秒)
    void setTargetMs(int ms);
    void setTimeBounds(double minSec, double maxSec);

    // 将耗时格式化为带单位的字符串 (ns/us/ms/s)
    static std::string formatNs(int64_t ns);

    // 打印单次结果 (可单独调用)
    static void printResult(const JZBenchmarkResult& r);

private:
    int m_targetMs;
    double m_minSec;
    double m_maxSec;
};

#endif
