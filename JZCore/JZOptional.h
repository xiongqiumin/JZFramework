#ifndef JZ_OPTIONAL_H_
#define JZ_OPTIONAL_H_
#include <memory>
#include <type_traits>
#include <utility>
#include "JZGlobal.h"

template <typename T>
class JZFRAMEWORK_API JZOptional
{
    static_assert(!std::is_reference<T>::value, "JZOptional does not support reference types");
    static_assert(!std::is_same<T, void>::value, "Use JZOptional<void> specialization for void");

public:
    // 创建空值（相当于 std::nullopt）
    static JZOptional None()
    {
        return JZOptional();
    }

    // 创建有值（左值）
    static JZOptional Some(const T& value)
    {
        return JZOptional(value);
    }

    // 创建有值（右值，移动优化）
    static JZOptional Some(T&& value)
    {
        return JZOptional(std::move(value));
    }

    // 默认构造 = 空值
    JZOptional() : m_hasValue(false)
    {
    }

    // 左值拷贝构造
    JZOptional(const T& value) : m_hasValue(true)
    {
        new (static_cast<void*>(std::addressof(m_data))) T(value);
    }

    // 右值移动构造
    JZOptional(T&& value) : m_hasValue(true)
    {
        new (static_cast<void*>(std::addressof(m_data))) T(std::move(value));
    }

    // 拷贝构造（深拷贝）
    JZOptional(const JZOptional& other) : m_hasValue(other.m_hasValue)
    {
        if (m_hasValue)
        {
            new (static_cast<void*>(std::addressof(m_data))) T(other.value());
        }
    }

    // 拷贝赋值
    JZOptional& operator=(const JZOptional& other)
    {
        if (this != &other)
        {
            clear();
            m_hasValue = other.m_hasValue;
            if (m_hasValue)
            {
                new (static_cast<void*>(std::addressof(m_data))) T(other.value());
            }
        }
        return *this;
    }

    // 移动构造
    JZOptional(JZOptional&& other) : m_hasValue(other.m_hasValue)
    {
        if (m_hasValue)
        {
            new (static_cast<void*>(std::addressof(m_data))) T(std::move(other.value()));
            other.clear();
        }
    }

    // 移动赋值
    JZOptional& operator=(JZOptional&& other) 
    {
        if (this != &other)
        {
            clear();
            m_hasValue = other.m_hasValue;
            if (m_hasValue)
            {
                new (static_cast<void*>(std::addressof(m_data))) T(std::move(other.value()));
                other.clear();
            }
        }
        return *this;
    }

    // 析构
    ~JZOptional()
    {
        clear();
    }

    const T* operator->() const
    {
        JZ_ASSERT(m_hasValue && "JZOptional::operator-> called on None");
        return reinterpret_cast<const T*>(&m_data);
    }

    T* operator->()
    {
        JZ_ASSERT(m_hasValue && "JZOptional::operator-> called on None");
        return reinterpret_cast<T*>(&m_data);
    }

    explicit operator bool() const noexcept
    {
        return isSome();
    }

    // 是否有值
    bool isSome() const
    {
        return m_hasValue;
    }

    // 是否空
    bool isNone() const
    {
        return !m_hasValue;
    }

    // 获取值（必须 isSome 才能调用）
    const T& value() const
    {
        JZ_ASSERT(m_hasValue && "JZOptional::value() called on None");
        return *reinterpret_cast<const T*>(&m_data);
    }

    // 获取值（非 const 版本）
    T& value()
    {
        JZ_ASSERT(m_hasValue && "JZOptional::value() called on None");
        return *reinterpret_cast<T*>(&m_data);
    }

    // 获取值或默认值
    T valueOr(const T& defaultValue) const
    {
        return m_hasValue ? value() : defaultValue;
    }

    // 重置为空
    void reset()
    {
        clear();
    }

private:
    // 安全释放
    void clear()
    {
        if (m_hasValue)
        {
            reinterpret_cast<T*>(&m_data)->~T();
            m_hasValue = false;
        }
    }

private:
    bool m_hasValue = false;
    alignas(T) unsigned char m_data[sizeof(T)];  // 使用 alignas 替代 aligned_storage
};

// void 特化: 仅声明, 成员实现见 JZOptional.cpp
template <>
class JZFRAMEWORK_API JZOptional<void>
{
public:
    static JZOptional None();
    static JZOptional Some();
    JZOptional();
    explicit operator bool() const noexcept;
    bool isSome() const;
    bool isNone() const;
    void reset();

private:
    bool m_hasValue;
};

// 拉入与 JZDebug 的流式输出算子 (定义见 detail/JZDebugDetail.h, 仅当 JZDebug 也可见时生成).
#include "detail/JZDebugDetail.h"

#endif  // !JZ_OPTIONAL_H_