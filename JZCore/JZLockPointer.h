#ifndef JZ_LOCK_POINTER_H_
#define JZ_LOCK_POINTER_H_

#include "JZGlobal.h"

template<typename T>
class JZLockPointer 
{
public:
    // RAII 守卫类：持有锁，并提供指针访问
    class LockGuard
    {
    public:
        LockGuard(JZLockPointer* parent, T* raw_ptr) : m_parent(parent), m_raw_ptr(raw_ptr)
        {
        }

        LockGuard(LockGuard&& other)
        {
            m_raw_ptr = other.m_raw_ptr;
            m_parent = other.m_parent;
            other.m_parent = nullptr;
            other.m_raw_ptr = nullptr;
        }

        ~LockGuard()
        {
            if (m_parent)
                m_parent->unlock();
        }

        // 指针访问操作符（直接返回原始指针，可能为 nullptr，用户需自行检查）
        T* operator->()
        {
            return m_raw_ptr;
        }
        const T* operator->() const
        {
            return m_raw_ptr;
        }

        T& operator*()
        {
            return *m_raw_ptr;
        }
        const T& operator*() const
        {
            return *m_raw_ptr;
        }

        // 显式获取原始指针
        T* get()
        {
            return m_raw_ptr;
        }
        const T* get() const
        {
            return m_raw_ptr;
        }

        // 检查指针是否有效
        explicit operator bool() const
        {
            return m_raw_ptr != nullptr;
        }

    private:
        JZ_DISABLE_COPY(LockGuard);

        T* m_raw_ptr;
        JZLockPointer* m_parent;
    };

    JZLockPointer()
        : m_mutex(std::make_shared<std::mutex>()),
        m_ptr(nullptr),m_type(Raw_Pointer) 
    {

    }

    // 裸指针构造
    JZLockPointer(T* ptr)
        : m_mutex(std::make_shared<std::mutex>()), 
        m_ptr(ptr),m_type(Raw_Pointer) {}

    // shared_ptr 构造
    JZLockPointer(std::shared_ptr<T> ptr)
        : m_mutex(std::make_shared<std::mutex>()), 
        m_shared(ptr),m_ptr(nullptr),m_type(Shared_Pointer) {}

    // weak_ptr 构造
    JZLockPointer(std::weak_ptr<T> ptr)
        : m_mutex(std::make_shared<std::mutex>()), 
        m_weak(ptr),m_ptr(nullptr),m_type(Weak_Pointer) {}

    // 拷贝构造（可跨线程传递）
    JZLockPointer(const JZLockPointer&) = default;
    JZLockPointer& operator=(const JZLockPointer&) = default;

    // 锁定并返回守卫
    LockGuard lock()
    {
        m_mutex->lock();

        T* p = nullptr;
        if (m_type == Raw_Pointer)
        {
            p = m_ptr;
        }
        else if (m_type == Shared_Pointer)
        {
            p = m_shared.get();
        }
        else
        {
            m_shared = m_weak.lock();
            p = m_shared.get();
        }
        return std::move(LockGuard(this, p));
    }

    void unlock()
    {
        if (m_type == Weak_Pointer)
            m_shared = std::shared_ptr<T>();
        m_mutex->unlock();
    }

private:
    enum PointerType{
        Raw_Pointer,
        Shared_Pointer,
        Weak_Pointer,
    };
    std::shared_ptr<std::mutex> m_mutex;
    PointerType m_type;
    T* m_ptr;
    std::shared_ptr<T> m_shared;
    std::weak_ptr<T> m_weak;
};

#endif
