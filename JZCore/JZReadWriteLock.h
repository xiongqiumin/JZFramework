#ifndef JZ_READ_WRITE_LOCK_H_
#define JZ_READ_WRITE_LOCK_H_
#include "JZGlobal.h"
#include "JZMutex.h"
#include "JZConditionVariable.h"

// 读写锁：允许多个读线程并发，写线程独占。
// 基于 JZMutex + JZConditionVariable 实现，跨平台（普通系统 / FreeRTOS / RT-Thread）。
class JZFRAMEWORK_API JZReadWriteLock
{
public:
    JZReadWriteLock();
    ~JZReadWriteLock();

    // 读锁
    void lockForRead();
    bool tryLockForRead();
    void unlockRead();

    // 写锁
    void lockForWrite();
    bool tryLockForWrite();
    void unlockWrite();

protected:
    JZ_DISABLE_COPY(JZReadWriteLock)

private:
    mutable JZMutex m_mutex;
    JZConditionVariable m_readerCv;
    JZConditionVariable m_writerCv;

    int m_readers;      // 当前持有读锁的线程数
    bool m_writer;      // 是否有写者持有/等待独占
    int m_writersWait;  // 等待写锁的线程数
};

// 读写锁作用域辅助类（读锁）
class JZFRAMEWORK_API JZReadLocker
{
public:
    explicit JZReadLocker(JZReadWriteLock *lock);
    ~JZReadLocker();

    JZReadWriteLock *lock() const;
    void relock();
    void unlock();

protected:
    JZ_DISABLE_COPY(JZReadLocker)

private:
    JZReadWriteLock *m_lock;
    bool m_locked;
};

// 读写锁作用域辅助类（写锁）
class JZFRAMEWORK_API JZWriteLocker
{
public:
    explicit JZWriteLocker(JZReadWriteLock *lock);
    ~JZWriteLocker();

    JZReadWriteLock *lock() const;
    void relock();
    void unlock();

protected:
    JZ_DISABLE_COPY(JZWriteLocker)

private:
    JZReadWriteLock *m_lock;
    bool m_locked;
};

#endif // ! JZ_READ_WRITE_LOCK_H_
