#ifndef JZ_MUTEX_POINTER_H_
#define JZ_MUTEX_POINTER_H_

#include "JZGlobal.h"
#include "JZMutex.h"
#include <functional>

//跨线程使用
// 类似 Rust Mutex<T>：用 JZMutex 保护对指针的访问
// lock() 返回 JZMutexPointerGuard，RAII 析构时通过 cleanup 回调统一解锁/清理
template<typename T>
class JZMutexPointerGuard
{
public:
    JZMutexPointerGuard(T* raw_ptr, std::function<void()> cleanup = nullptr)
        : m_raw_ptr(raw_ptr), m_cleanup(std::move(cleanup))
    {
    }

    // 移动构造：转移所有权，源置空以避免重复解锁
    JZMutexPointerGuard(JZMutexPointerGuard&& other) noexcept
        : m_raw_ptr(other.m_raw_ptr), m_cleanup(std::move(other.m_cleanup))
    {
        other.m_raw_ptr = nullptr;
        other.m_cleanup = nullptr;
    }

    ~JZMutexPointerGuard()
    {
        if (m_cleanup)
            m_cleanup();
    }

    T* get()
    {
        return m_raw_ptr;
    }

    // 主动解锁：执行 cleanup 并置空，后续 get() 返回 nullptr，析构不再重复解锁
    void unlock()
    {
        if (m_cleanup)
        {
            m_cleanup();
            m_cleanup = nullptr;
            m_raw_ptr = nullptr;
        }
    }

private:
    JZ_DISABLE_COPY(JZMutexPointerGuard);

    T* m_raw_ptr;
    std::function<void()> m_cleanup;
};

// 裸指针版本
template<typename T>
class JZMutexRawPointer
{
public:
    JZMutexRawPointer()
        : m_mutex(std::make_shared<JZMutex>()), m_ptr(nullptr)
    {
    }

    JZMutexRawPointer(T* ptr)
        : m_mutex(std::make_shared<JZMutex>()), m_ptr(ptr)
    {
    }

    // 拷贝构造（可跨线程传递）
    JZMutexRawPointer(const JZMutexRawPointer&) = default;
    JZMutexRawPointer& operator=(const JZMutexRawPointer&) = default;

    // 锁定并返回守卫
    JZMutexPointerGuard<T> lock()
    {
        m_mutex->lock();
        return std::move(JZMutexPointerGuard<T>(m_ptr, [this]() { m_mutex->unlock(); }));
    }

private:
    std::shared_ptr<JZMutex> m_mutex;
    T* m_ptr;
};

// shared_ptr 版本
template<typename T>
class JZMutexSharedPointer
{
public:
    JZMutexSharedPointer()
        : m_mutex(std::make_shared<JZMutex>()), m_ptr(nullptr)
    {
    }

    JZMutexSharedPointer(std::shared_ptr<T> ptr)
        : m_mutex(std::make_shared<JZMutex>()), m_ptr(ptr)
    {
    }

    // 拷贝构造（可跨线程传递）
    JZMutexSharedPointer(const JZMutexSharedPointer&) = default;
    JZMutexSharedPointer& operator=(const JZMutexSharedPointer&) = default;

    // 锁定并返回守卫
    JZMutexPointerGuard<T> lock()
    {
        m_mutex->lock();
        return std::move(JZMutexPointerGuard<T>(m_ptr.get(), [this]() { m_mutex->unlock(); }));
    }

private:
    std::shared_ptr<JZMutex> m_mutex;
    std::shared_ptr<T> m_ptr;
};

// weak_ptr 版本
template<typename T>
class JZMutexWeakPointer
{
public:
    JZMutexWeakPointer()
        : m_mutex(std::make_shared<JZMutex>())
    {
    }

    JZMutexWeakPointer(std::weak_ptr<T> ptr)
        : m_mutex(std::make_shared<JZMutex>()), m_weak(ptr)
    {
    }

    // 拷贝构造（可跨线程传递）
    JZMutexWeakPointer(const JZMutexWeakPointer&) = default;
    JZMutexWeakPointer& operator=(const JZMutexWeakPointer&) = default;

    // 锁定并返回守卫
    JZMutexPointerGuard<T> lock()
    {
        m_mutex->lock();
        m_shared = m_weak.lock();
        return std::move(JZMutexPointerGuard<T>(
            m_shared.get(),
            [this]() {
                m_shared = std::shared_ptr<T>();
                m_mutex->unlock();
            }));
    }

private:
    std::shared_ptr<JZMutex> m_mutex;
    std::weak_ptr<T> m_weak;
    std::shared_ptr<T> m_shared;   // 锁定时临时持有
};

#endif
