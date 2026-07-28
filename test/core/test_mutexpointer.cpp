#include "test_mutexpointer.h"

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "JZTest.h"
#include "JZMutexPointer.h"

namespace {

// 单线程基础: 裸指针可读写, 守卫析构后锁释放(可再次锁定而不死锁)
static bool mutex_raw_pointer_basic()
{
    int value = 0;
    JZMutexRawPointer<int> mp(&value);

    {
        auto g = mp.lock();
        JZ_TEST_NEQ(g.get(), nullptr);
        *g.get() = 10;
    }
    JZ_TEST_EQ(value, 10);

    // 再次锁定应成功(若未解锁会死锁), 并读到之前写入的值
    {
        auto g = mp.lock();
        JZ_TEST_EQ(*g.get(), 10);
    }
    return true;
}

// shared_ptr 版本: 锁定后可访问 pointee, 且通过原 shared_ptr 也可见
static bool mutex_shared_pointer_basic()
{
    auto sp = std::make_shared<int>(5);
    JZMutexSharedPointer<int> mp(sp);

    {
        auto g = mp.lock();
        JZ_TEST_NEQ(g.get(), nullptr);
        JZ_TEST_EQ(*g.get(), 5);
        *g.get() = 7;
    }
    JZ_TEST_EQ(*sp, 7);
    return true;
}

// weak_ptr 版本: 源存活时锁定得到有效指针, 且能看到最新值
static bool mutex_weak_pointer_valid()
{
    auto sp = std::make_shared<int>(1);
    JZMutexWeakPointer<int> mp(sp);

    {
        auto g = mp.lock();
        JZ_TEST_NEQ(g.get(), nullptr);
        JZ_TEST_EQ(*g.get(), 1);
        *g.get() = 2;
    }
    JZ_TEST_EQ(*sp, 2);
    return true;
}

// weak_ptr 版本: 源被销毁后锁定返回 nullptr
static bool mutex_weak_pointer_expired()
{
    std::weak_ptr<int> wp;
    {
        auto sp = std::make_shared<int>(9);
        wp = sp;
        JZMutexWeakPointer<int> mp(wp);
        auto g = mp.lock();
        JZ_TEST_NEQ(g.get(), nullptr);
        JZ_TEST_EQ(*g.get(), 9);
    } // sp 在此销毁

    JZMutexWeakPointer<int> mp(wp);
    auto g = mp.lock();
    JZ_TEST_EQ(g.get(), nullptr);  // 已过期
    return true;
}

// 拷贝: 拷贝共享同一把锁与同一指针, 可用于跨线程传递
static bool mutex_pointer_copy()
{
    int value = 0;
    JZMutexRawPointer<int> mp(&value);
    auto mp2 = mp;  // 拷贝

    {
        auto g = mp2.lock();
        *g.get() = 42;
    }
    JZ_TEST_EQ(value, 42);
    return true;
}

// 主动 unlock: 解锁后 get() 返回 nullptr, 析构不再重复解锁, 且可再次 lock()
static bool mutex_guard_unlock()
{
    int value = 0;
    JZMutexRawPointer<int> mp(&value);

    {
        auto g = mp.lock();
        *g.get() = 7;
        g.unlock();
        JZ_TEST_EQ(g.get(), nullptr);  // 解锁后指针置空
    } // 析构时 cleanup 已为空, 不会重复解锁(否则会死锁)

    // 主动解锁后, 后续可正常再次锁定
    {
        auto g = mp.lock();
        JZ_TEST_NEQ(g.get(), nullptr);
        JZ_TEST_EQ(*g.get(), 7);
    }
    return true;
}

// 并发自增: 若守卫未正确解锁, 会出现死锁/挂起
template<typename MutexPtr>
static void stress_increment(MutexPtr mp, int iterations)
{
    for (int i = 0; i < iterations; ++i)
    {
        auto g = mp.lock();
        int* p = g.get();
        int v = *p;
        std::this_thread::yield();  // 增大竞态窗口
        *p = v + 1;
    }
}

// 裸指针版本跨线程压力测试
static bool mutex_pointer_cross_thread()
{
    const int kThreads = 8;
    const int kIters = 2000;

    int counter = 0;
    JZMutexRawPointer<int> mp(&counter);

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i)
        threads.emplace_back(stress_increment<JZMutexRawPointer<int>>, mp, kIters);

    for (auto& t : threads)
        t.join();

    JZ_TEST_EQ(counter, kThreads * kIters);
    return true;
}

// shared_ptr 版本跨线程压力测试
static bool mutex_shared_pointer_cross_thread()
{
    const int kThreads = 8;
    const int kIters = 2000;

    auto sp = std::make_shared<int>(0);
    JZMutexSharedPointer<int> mp(sp);

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i)
        threads.emplace_back(stress_increment<JZMutexSharedPointer<int>>, mp, kIters);

    for (auto& t : threads)
        t.join();

    JZ_TEST_EQ(*sp, kThreads * kIters);
    return true;
}

} // namespace

void test_JZMutexPointer(JZTest *test)
{
    auto suite = std::make_shared<JZTestSuite>("JZMutexPointer");
    test->addSuite(suite);
    suite->addTestCase("raw_pointer_basic", mutex_raw_pointer_basic);
    suite->addTestCase("shared_pointer_basic", mutex_shared_pointer_basic);
    suite->addTestCase("weak_pointer_valid", mutex_weak_pointer_valid);
    suite->addTestCase("weak_pointer_expired", mutex_weak_pointer_expired);
    suite->addTestCase("pointer_copy", mutex_pointer_copy);
    suite->addTestCase("guard_unlock", mutex_guard_unlock);
    suite->addTestCase("cross_thread", mutex_pointer_cross_thread);
    suite->addTestCase("shared_cross_thread", mutex_shared_pointer_cross_thread);
}
