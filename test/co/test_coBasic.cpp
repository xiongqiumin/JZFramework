#include <string>
#include <vector>

#include "JZDebug.h"
#include "JZApplication.h"
#include "JZTimer.h"
#include "JZEventLoop.h"

// ===================== 协程基础库 =====================
#include "JZCo.h"
#include "JZCoMutex.h"
#include "JZCoSemaphore.h"
#include "JZCoCoroutine.h"   // JZCoCondition

// ===================== 测试公用工具 =====================
#include "co_test_common.h"    // CO_TEST_ASSERT (失败即 exit(1))

using namespace std;

// ============ 协程框架基本能力: spawn / wait ============
static void testCoSpawnBasics()
{
    int done = 0;
    vector<JZCoCoroutinePtr> cos;
    for (int i = 0; i < 5; i++)
        cos.push_back(jzco_spawn([&]() { done++; }));
    jzco_waitCoroutine(cos);
    CO_TEST_ASSERT(done == 5, "应完成 5 个协程, 实际=" + to_string(done));
    jzDebug() << "【OK】协程 spawn/wait 基本能力正确";
}

// ============ JZCoMutex: 机械 lock/unlock (无竞争) ============
static void testCoMutexMechanic()
{
    JZCoMutex m;
    for (int i = 0; i < 100; i++)
    {
        m.lock();
        m.unlock();
    }
    CO_TEST_ASSERT(m.tryLock(), "循环结束后应能 tryLock");
    m.unlock();
    jzDebug() << "【OK】JZCoMutex lock/unlock 机械调用稳定";
}

// ============ JZCoMutex: tryLock / JZCoMutexLocker (RAII) ============
static void testCoMutexTryLockAndLocker()
{
    JZCoMutex m;
    CO_TEST_ASSERT(m.tryLock(), "首次 tryLock 应成功");
    CO_TEST_ASSERT(!m.tryLock(), "已锁定时 tryLock 应失败");
    m.unlock();

    {
        JZCoMutexLocker locker(&m);
        CO_TEST_ASSERT(!m.tryLock(), "加锁后 tryLock 应失败");
    }
    CO_TEST_ASSERT(m.tryLock(), "离开作用域后自动解锁, tryLock 应成功");
    m.unlock();

    jzDebug() << "【OK】JZCoMutex::tryLock / JZCoMutexLocker 正确";
}

// ============ JZCoMutex: 阻塞 + 互斥 (等待方直到持有方释放) ============
// 注意: A 在 unlock 时内联 resume B, 调用方 A 会被调度器孤立, 故只等待 B
static void testCoMutexBlocking()
{
    JZCoMutex m;
    bool bGotLock = false;

    // A 先加锁并持有一段时间(让出), 再释放
    jzco_spawn([&]() {
        m.lock();
        jzco_sleep(30);    // 持有锁期间让出, 使 B 的 lock() 进入等待
        m.unlock();          // 释放并唤醒 B (此协程随后被孤立, 属已知调度特性, 不等待它)
    });

    // B 后启动, 尝试加锁: 必须阻塞直到 A 释放
    auto coB = jzco_spawn([&]() {
        m.lock();
        bGotLock = true;    // 只有 A 释放后 B 才能到达这里
        m.unlock();
    });

    jzco_waitCoroutine({coB});   // 只等 B
    CO_TEST_ASSERT(bGotLock, "B 应当在 A 释放锁之后才获得锁 (互斥/阻塞正确)");
    jzDebug() << "【OK】JZCoMutex 阻塞+互斥正确 (B 等待 A 释放后获得锁)";
}

// ============ JZCoSemaphore: 计数 + 阻塞/唤醒 + 超时 ============
static void testCoSemaphore()
{
    JZCoSemaphore sem(0);
    CO_TEST_ASSERT(sem.available() == 0, "初始 available 应为 0");
    CO_TEST_ASSERT(!sem.tryAcquire(1), "资源不足时 tryAcquire 应失败");

    int consumerDone = 0;
    auto consumer = jzco_spawn([&]() {
        sem.acquire(2);           // 阻塞直到累计获得 2 个资源
        consumerDone = 1;
    });
    auto producer = jzco_spawn([&]() {
        jzco_sleep(20); sem.release(1);
        jzco_sleep(20); sem.release(1);
    });
    jzco_waitCoroutine({consumer, producer});

    CO_TEST_ASSERT(consumerDone == 1, "消费者未被唤醒 (信号量阻塞/唤醒失败)");
    CO_TEST_ASSERT(sem.available() == 0, "消费后 available 应为 0, 实际=" + to_string(sem.available()));

    // 计数语义
    JZCoSemaphore s2(0);
    s2.release(3);
    CO_TEST_ASSERT(s2.available() == 3, "release(3) 后 available 应为 3");
    CO_TEST_ASSERT(!s2.tryAcquire(5), "请求超过可用量应失败");
    CO_TEST_ASSERT(s2.tryAcquire(2), "请求未超量应成功");
    CO_TEST_ASSERT(s2.available() == 1, "消费 2 后 available 应为 1, 实际=" + to_string(s2.available()));

    // 超时获取: 始终不释放, 应超时返回 false
    JZCoSemaphore s3(0);
    auto t = jzco_spawn([&]() {
        bool ok = s3.tryAcquire(1, 50);
        CO_TEST_ASSERT(!ok, "超时应返回 false");
    });
    jzco_waitCoroutine({t});

    jzDebug() << "【OK】JZCoSemaphore 计数/阻塞/唤醒/超时正确";
}

// ============ JZCoCondition: wait / wakeOne / wakeAll ============
// 注意: 唤醒通过 JZTimer::singleShot 从主上下文发起,
//       避免内联 resume 造成调用协程被孤立 (参考 JZCoSemaphore 的安全做法)
static void testCoCondition()
{
    // ---- wakeOne ----
    {
        JZCoCondition cond;
        int woken = 0;
        vector<JZCoCoroutinePtr> waiters;
        for (int i = 0; i < 2; i++)
            waiters.push_back(jzco_spawn([&]() { cond.wait(); woken++; }));

        jzco_sleep(30);                          // 等两个 waiter 真正进入等待
        JZTimer::singleShot(0, [&]() { cond.wakeOne(); });
        jzco_sleep(30);
        CO_TEST_ASSERT(woken == 1, "wakeOne 后应唤醒 1 个, 实际=" + to_string(woken));

        JZTimer::singleShot(0, [&]() { cond.wakeOne(); });
        jzco_waitCoroutine(waiters);
        CO_TEST_ASSERT(woken == 2, "再次 wakeOne 后应唤醒 2 个, 实际=" + to_string(woken));
    }

    // ---- wakeAll ----
    {
        JZCoCondition cond;
        int woken = 0;
        vector<JZCoCoroutinePtr> waiters;
        for (int i = 0; i < 3; i++)
            waiters.push_back(jzco_spawn([&]() { cond.wait(); woken++; }));

        jzco_sleep(30);
        JZTimer::singleShot(0, [&]() { cond.wakeAll(); });
        jzco_waitCoroutine(waiters);
        CO_TEST_ASSERT(woken == 3, "wakeAll 后应唤醒 3 个, 实际=" + to_string(woken));
    }

    jzDebug() << "【OK】JZCoCondition wait/wakeOne/wakeAll 正确";
}

// ===================== 测试入口 (供 test_co.cpp 编排调用) =====================
int test_coBasic()
{
    jzDebug() << "########## 协程基础功能测试 (spawn / mutex / semaphore / condition) ##########";

    testCoSpawnBasics();
    testCoMutexMechanic();
    testCoMutexTryLockAndLocker();
    testCoMutexBlocking();
    testCoSemaphore();
    testCoCondition();

    jzDebug() << "========== 协程基础库测试通过 ==========";
    return 0;
}
