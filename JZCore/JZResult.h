#ifndef JZ_RESULT_H_
#define JZ_RESULT_H_

#include <cassert>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

#include "JZGlobal.h"

class JZResultStatus
{
public:
    int code = 1;
    std::string message;
};

template <typename T>
class JZResult
{
    static_assert(!std::is_reference<T>::value, "JZResult does not support reference types");
    static_assert(!std::is_same<T, void>::value, "Use JZResult<void> specialization for void");

public:
    // 静态创建成功结果（左值）
    static JZResult Ok(const T& value)
    {
        JZResult ret;
        ret.m_isOk = true;
        ret.m_code = 0;
        new (&ret.m_data) T(value);
        return ret;
    }

    // 静态创建成功结果（右值，移动语义）
    static JZResult Ok(T&& value)
    {
        JZResult ret;
        ret.m_isOk = true;
        ret.m_code = 0;
        new (&ret.m_data) T(std::move(value));
        return ret;
    }

    // 静态创建错误结果
    static JZResult Error(const std::string& error_msg)
    {
        return Error(1, error_msg);
    }

    static JZResult Error(int code, const std::string& error_msg)
    {
        JZResult ret;
        ret.m_isOk = false;
        ret.m_code = code;
        ret.m_error = error_msg;
        return ret;
    }

    // 禁止默认构造（必须显式 Ok 或 Error）
    JZResult()
    {
        m_code = 1;
        m_error = "undefine";
        m_isOk = false;
    }

    // 拷贝构造（深拷贝）
    JZResult(const JZResult& other) : m_isOk(other.m_isOk), m_code(other.m_code), m_error(other.m_error)
    {
        if (m_isOk)
        {
            new (&m_data) T(*other.getPtr());
        }
    }

    JZResult(JZResultStatus error)
    {
        m_code = error.code;
        m_error = error.message;
        m_isOk = false;
    }

    // 拷贝赋值
    JZResult& operator=(const JZResult& other)
    {
        if (this != &other)
        {
            clear();
            m_isOk = other.m_isOk;
            m_code = other.m_code;
            m_error = other.m_error;
            if (m_isOk)
            {
                new (&m_data) T(*other.getPtr());
            }
        }
        return *this;
    }

    // 移动构造
    JZResult(JZResult&& other) : m_isOk(other.m_isOk), m_code(other.m_code), m_error(std::move(other.m_error))
    {
        if (m_isOk)
        {
            new (&m_data) T(std::move(*other.getPtr()));
            other.clear();
        }
        other.m_isOk = false;
        other.m_code = -1;
    }

    // 移动赋值
    JZResult& operator=(JZResult&& other)
    {
        if (this != &other)
        {
            clear();
            m_isOk = other.m_isOk;
            m_code = other.m_code;
            m_error = std::move(other.m_error);

            if (m_isOk)
            {
                new (&m_data) T(std::move(*other.getPtr()));
                other.clear();
            }
            other.m_isOk = false;
            other.m_code = -1;
        }
        return *this;
    }

    // 析构函数
    ~JZResult()
    {
        clear();
    }

    // 状态判断
    bool isOk() const
    {
        return m_isOk;
    }

    bool isError() const
    {
        return !m_isOk;
    }

    // 安全访问数据
    const T& value() const
    {
        JZ_ASSERT(isOk() && "JZResult::value() called on Error");
        return *getPtr();
    }

    T& value()
    {
        JZ_ASSERT(isOk() && "JZResult::value() called on Error");
        return *getPtr();
    }

    // 获取值或默认值
    T valueOr(const T& defaultValue) const
    {
        return isOk() ? value() : defaultValue;
    }

    // 错误信息
    int errorCode() const
    {
        return m_code;
    }

    const std::string& errorMessage() const
    {
        assert(isError() && "JZResult::errorMessage() called on Ok");
        return m_error;
    }

    JZResultStatus status() const
    {
        JZResultStatus err;
        err.code = m_code;
        err.message = m_error;
        return err;
    }

private:
    void clear()
    {
        if (m_isOk)
        {
            getPtr()->~T();
            m_isOk = false;
        }
    }

    const T* getPtr() const
    {
        return reinterpret_cast<const T*>(&m_data);
    }

    T* getPtr()
    {
        return reinterpret_cast<T*>(&m_data);
    }

private:
    alignas(T) unsigned char m_data[sizeof(T)];  // 使用 alignas 替代 aligned_storage
    bool m_isOk;
    int m_code;
    std::string m_error;
};

// void 特化
template <>
class JZResult<void>
{
public:
    static JZResult Ok()
    {
        JZResult ret;
        ret.m_isOk = true;
        ret.m_code = 0;
        return ret;
    }

    static JZResult Error(const std::string& error_msg)
    {
        return Error(1, error_msg);
    }

    static JZResult Error(int code, const std::string& error_msg)
    {
        JZResult ret;
        ret.m_isOk = false;
        ret.m_code = code;
        ret.m_error = error_msg;
        return ret;
    }

    JZResult(JZResultStatus error)
    {
        m_code = error.code;
        m_error = error.message;
        m_isOk = false;
    }

    JZResult()
    {
        m_isOk = false;
        m_code = 1;
    }

    bool isOk() const
    {
        return m_isOk;
    }

    bool isError() const
    {
        return !m_isOk;
    }

    explicit operator bool() const
    {
        return m_isOk;
    }

    int errorCode() const
    {
        return m_code;
    }

    const std::string& errorMessage() const
    {
        assert(isError() && "JZResult<void>::errorMessage() called on Ok");
        return m_error;
    }
    
    JZResultStatus status() const
    {
        JZResultStatus err;
        err.code = m_code;
        err.message = m_error;
        return err;
    }

private:
    bool m_isOk = false;
    int m_code = 0;
    std::string m_error;
};

// 拉入与 JZDebug 的流式输出算子 (定义见 detail/JZDebugDetail.h, 仅当 JZDebug 也可见时生成).
#include "detail/JZDebugDetail.h"

#endif  // !JZ_RESULT_H_