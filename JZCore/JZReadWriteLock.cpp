#include "JZReadWriteLock.h"

// -------------------------- JZReadWriteLock --------------------------
JZReadWriteLock::JZReadWriteLock()
    : m_readers(0)
    , m_writer(false)
    , m_writersWait(0)
{
}

JZReadWriteLock::~JZReadWriteLock()
{
}

// 读锁：无写者（持有或等待）时允许进入
void JZReadWriteLock::lockForRead()
{
    JZMutexLocker locker(&m_mutex);
    m_readerCv.wait(locker, [this]() {
        return !m_writer && m_writersWait == 0;
    });
    ++m_readers;
}

bool JZReadWriteLock::tryLockForRead()
{
    JZMutexLocker locker(&m_mutex);
    if (m_writer || m_writersWait > 0)
        return false;
    ++m_readers;
    return true;
}

void JZReadWriteLock::unlockRead()
{
    JZMutexLocker locker(&m_mutex);
    JZ_ASSERT(m_readers > 0);
    --m_readers;
    if (m_readers == 0)
    {
        if (m_writersWait > 0)
            m_writerCv.notify_one();   // 有写者等待：最后一个读者离开，唤醒写者
        else
            m_readerCv.notify_all();   // 无写者等待：唤醒阻塞的读线程（防御性兜底）
    }
}

// 写锁：独占，需等待所有读者离开且无其他写者
void JZReadWriteLock::lockForWrite()
{
    JZMutexLocker locker(&m_mutex);
    ++m_writersWait;
    m_writerCv.wait(locker, [this]() {
        return !m_writer && m_readers == 0;
    });
    --m_writersWait;
    m_writer = true;
}

bool JZReadWriteLock::tryLockForWrite()
{
    JZMutexLocker locker(&m_mutex);
    if (m_writer || m_readers > 0)
        return false;
    m_writer = true;
    return true;
}

void JZReadWriteLock::unlockWrite()
{
    JZMutexLocker locker(&m_mutex);
    JZ_ASSERT(m_writer);
    m_writer = false;
    if (m_writersWait > 0)
        m_writerCv.notify_one();   // 优先唤醒写者，避免写者饥饿
    else
        m_readerCv.notify_all();   // 无写者等待，唤醒所有读者
}

// -------------------------- JZReadLocker --------------------------
JZReadLocker::JZReadLocker(JZReadWriteLock *lock)
    : m_lock(lock)
{
    JZ_ASSERT(m_lock);
    m_lock->lockForRead();
    m_locked = true;
}

JZReadLocker::~JZReadLocker()
{
    if (m_locked)
        m_lock->unlockRead();
}

JZReadWriteLock *JZReadLocker::lock() const
{
    return m_lock;
}

void JZReadLocker::relock()
{
    m_lock->lockForRead();
    m_locked = true;
}

void JZReadLocker::unlock()
{
    m_lock->unlockRead();
    m_locked = false;
}

// -------------------------- JZWriteLocker --------------------------
JZWriteLocker::JZWriteLocker(JZReadWriteLock *lock)
    : m_lock(lock)
{
    JZ_ASSERT(m_lock);
    m_lock->lockForWrite();
    m_locked = true;
}

JZWriteLocker::~JZWriteLocker()
{
    if (m_locked)
        m_lock->unlockWrite();
}

JZReadWriteLock *JZWriteLocker::lock() const
{
    return m_lock;
}

void JZWriteLocker::relock()
{
    m_lock->lockForWrite();
    m_locked = true;
}

void JZWriteLocker::unlock()
{
    m_lock->unlockWrite();
    m_locked = false;
}
