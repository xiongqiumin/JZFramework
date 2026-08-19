#include "test_rwlock.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "JZTest.h"
#include "JZReadWriteLock.h"
#include "JZThread.h"

namespace {

// 并发执行指定次数迭代, 期间监测某个计数是否违反"最大值<=1"的约束
// 用于验证写者互斥: 同一时刻至多一个写者进入临界区
static void run_writer_exclusive(JZReadWriteLock &lock, std::atomic<int> &active,
                                 std::atomic<int> &maxActive, std::atomic<bool> &violated,
                                 int iterations, int workNs)
{
    for (int i = 0; i < iterations; ++i) {
        lock.lockForWrite();
        int cur = ++active;
        if (cur > maxActive) maxActive = cur;
        if (cur > 1) violated = true;   // 同一时刻出现 >1 个写者 => 写互斥被破坏
        if (workNs > 0) std::this_thread::sleep_for(std::chrono::nanoseconds(workNs));
        --active;
        lock.unlockWrite();
    }
}

} // namespace

// ============================ JZReadWriteLock ============================

// 读锁可并发: 两个线程同时持有读锁时 activeReaders 可达到 2
static bool rwlock_read_concurrent()
{
    JZReadWriteLock lock;
    std::atomic<int> readers{0};
    std::atomic<int> maxReaders{0};
    std::atomic<bool> reachedTwo{false};

    auto reader = [&]() {
        lock.lockForRead();
        int cur = ++readers;
        if (cur > maxReaders) maxReaders = cur;
        if (cur >= 2) reachedTwo = true;
        JZThread::msleep(10);
        --readers;
        lock.unlockRead();
    };

    std::thread t1(reader), t2(reader);
    t1.join();
    t2.join();

    JZ_TEST_TRUE(reachedTwo.load());      // 两个读者应能并发进入
    JZ_TEST_TRUE(maxReaders.load() >= 2);
    JZ_TEST_EQ(readers.load(), 0);
    return true;
}

// 写者互斥: 同一时刻至多一个写者进入临界区
static bool rwlock_write_exclusive()
{
    JZReadWriteLock lock;
    std::atomic<int> active{0};
    std::atomic<int> maxActive{0};
    std::atomic<bool> violated{false};

    const int kThreads = 4;
    const int kIters = 200;
    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i)
        threads.emplace_back(run_writer_exclusive, std::ref(lock), std::ref(active),
                             std::ref(maxActive), std::ref(violated), kIters, 1000);
    for (auto &t : threads) t.join();

    JZ_TEST_FALSE(violated.load());       // 从未出现两个写者同时进入
    JZ_TEST_LE(maxActive.load(), 1);
    JZ_TEST_EQ(active.load(), 0);
    return true;
}

// 读写互斥: 写者持锁时, 读者必须等待; 反之读者持锁时, 写者必须等待
static bool rwlock_read_write_exclusive()
{
    JZReadWriteLock lock;
    std::atomic<bool> writerInside{false};
    std::atomic<bool> readerDuringWrite{false};
    std::atomic<bool> writerDone{false};
    std::atomic<bool> readDone{false};

    std::thread writer([&]() {
        lock.lockForWrite();
        writerInside = true;
        JZThread::msleep(30);
        writerInside = false;
        writerDone = true;
        lock.unlockWrite();
    });

    // 读者线程: 若在写者持锁期间进入临界区, 则读写互斥被破坏
    std::thread reader([&]() {
        lock.lockForRead();
        if (writerInside.load()) readerDuringWrite = true; // 写者还没离开, 读者却进来了
        readDone = true;
        lock.unlockRead();
    });

    writer.join();
    reader.join();

    JZ_TEST_FALSE(readerDuringWrite.load());  // 读者不能闯入写者临界区
    JZ_TEST_TRUE(writerDone.load());
    JZ_TEST_TRUE(readDone.load());
    return true;
}

// 写优先: 一旦有写者在等待, 新到达的读者必须让路,
// 从而保证写者不会被持续到来的读者"饿死"
static bool rwlock_writer_preference()
{
    JZReadWriteLock lock;

    // 先让一个读者长时间持有读锁, 制造"读者占用中"的环境
    std::atomic<bool> holdReaderReleased{false};
    std::thread holder([&]() {
        lock.lockForRead();
        // 保持读锁, 直到写者完成
        while (!holdReaderReleased.load())
            JZThread::msleep(1);
        lock.unlockRead();
    });

    // 等待持有者真正拿到读锁
    JZThread::msleep(20);

    // 记录: 写者获取写锁的时刻, 以及期间是否有新读者进入
    std::atomic<bool> writerGotLock{false};
    std::atomic<bool> newReaderDuringWait{false};
    std::atomic<bool> writerCompleted{false};

    // 写者线程: 等待直到所有读者释放后获取写锁
    std::thread writer([&]() {
        lock.lockForWrite();
        writerGotLock = true;
        JZThread::msleep(10);
        writerCompleted = true;   // 先标记完成, 再释放锁, 避免读者被唤醒时的竞态误判
        lock.unlockWrite();
    });

    // 等待写者开始等待(给写者一点时间进入等待队列)
    JZThread::msleep(20);

    // 新读者线程: 在写者已经排队等待时尝试进入
    // 写优先 => 新读者应被阻塞, 不能抢在写者之前拿到读锁
    std::atomic<bool> newReaderEntered{false};
    std::thread newReader([&]() {
        lock.lockForRead();
        if (!writerCompleted.load()) newReaderDuringWait = true; // 写者还没完成, 新读者却进入了
        newReaderEntered = true;
        lock.unlockRead();
    });

    // 等新读者线程完全启动并阻塞在 lockForRead
    JZThread::msleep(20);

    // 释放持有读锁的读者
    holdReaderReleased = true;
    holder.join();

    writer.join();
    newReader.join();

    // 写者优先: 写者必须先于新读者获得锁
    JZ_TEST_TRUE(writerGotLock.load());
    JZ_TEST_TRUE(writerCompleted.load());
    JZ_TEST_TRUE(newReaderEntered.load());
    JZ_TEST_FALSE(newReaderDuringWait.load()); // 新读者不能在写者完成前进入
    return true;
}

// tryLockForRead / tryLockForWrite: 非阻塞, 冲突时立即返回 false
static bool rwlock_trylock()
{
    JZReadWriteLock lock;

    // 无竞争时 tryLock 成功
    JZ_TEST_TRUE(lock.tryLockForWrite());
    // 已持写锁, 再 tryLockForRead 应失败
    JZ_TEST_FALSE(lock.tryLockForRead());
    JZ_TEST_FALSE(lock.tryLockForWrite());
    lock.unlockWrite();

    // 读锁可被多个 tryLockForRead 获取
    JZ_TEST_TRUE(lock.tryLockForRead());
    JZ_TEST_TRUE(lock.tryLockForRead());
    // 持有读锁时 tryLockForWrite 应失败
    JZ_TEST_FALSE(lock.tryLockForWrite());
    lock.unlockRead();
    lock.unlockRead();

    // 释放后 tryLockForWrite 又成功
    JZ_TEST_TRUE(lock.tryLockForWrite());
    lock.unlockWrite();
    return true;
}

// 压测: 多个读者 + 写者并发, 验证互斥约束始终成立且无死锁/挂起
static bool rwlock_stress()
{
    JZReadWriteLock lock;
    std::atomic<int> activeWriters{0};
    std::atomic<int> activeReaders{0};
    std::atomic<bool> violated{false};

    const int kReaderThreads = 4;
    const int kWriterThreads = 2;
    const int kIters = 500;

    std::atomic<int> finishCount{0};
    std::atomic<int> total{0};
    const int expected = kReaderThreads + kWriterThreads;

    std::vector<std::thread> threads;

    for (int i = 0; i < kReaderThreads; ++i) {
        threads.emplace_back([&]() {
            for (int n = 0; n < kIters; ++n) {
                lock.lockForRead();
                ++activeReaders;
                if (activeWriters.load() > 0) violated = true; // 读者与写者并存 => 互斥被破坏
                total.fetch_add(1);
                --activeReaders;
                lock.unlockRead();
            }
            finishCount.fetch_add(1);
        });
    }

    for (int i = 0; i < kWriterThreads; ++i) {
        threads.emplace_back([&]() {
            for (int n = 0; n < kIters; ++n) {
                lock.lockForWrite();
                int w = ++activeWriters;
                if (activeReaders.load() > 0) violated = true; // 写者与读者并存
                if (w > 1) violated = true;                    // 两个写者并存
                total.fetch_add(1);
                --activeWriters;
                lock.unlockWrite();
            }
            finishCount.fetch_add(1);
        });
    }

    for (auto &t : threads) t.join();

    JZ_TEST_FALSE(violated.load());
    JZ_TEST_EQ(finishCount.load(), expected);
    JZ_TEST_EQ(total.load(), (kReaderThreads + kWriterThreads) * kIters);
    return true;
}

// ============================ 测试入口 ============================

void test_JZReadWriteLock(JZTest *test)
{
    auto suite = std::make_shared<JZTestSuite>("JZReadWriteLock");
    test->addSuite(suite);
    suite->addTestCase("read_concurrent", rwlock_read_concurrent);
    suite->addTestCase("write_exclusive", rwlock_write_exclusive);
    suite->addTestCase("read_write_exclusive", rwlock_read_write_exclusive);
    suite->addTestCase("writer_preference", rwlock_writer_preference);
    suite->addTestCase("trylock", rwlock_trylock);
    suite->addTestCase("stress", rwlock_stress);
}
